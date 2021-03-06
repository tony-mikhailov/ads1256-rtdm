/**
 * Copyright (C) 2017 Piotr Piórkowski <qba100@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "../include/ads1256-rtdm.h"


#ifdef __CDT_PARSER__
#define __init
#define __exit
#define MODULE_VERSION(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
#define module_init(x)
#define module_exit(x)
#define module_param(x)
#define MODULE_PARM_DESC(x)


#endif


static short drdy_gpio_pin = ADS1256_DEFAULT_DRDY_GPIO_PIN;
static short reset_gpio_pin = ADS1256_DEFAULT_RESET_GPIO_PIN;

static rtdm_task_t ads1256_task_agent;
static rtdm_task_t ads1256_search_ac_task;
static struct rtdm_device ads1256_adc_devices[ADS1256_ADC_DEVICES_NUMBER];
static ads1256_dev_context_t *ads1256_ads_open_devices[ADS1256_ADC_DEVICES_NUMBER];
static rtdm_lock_t global_lock;
static rtdm_lock_t close_lock;

static struct rtdm_spi_remote_slave *active_cs;
static struct rtdm_spi_master *rtdm_master;
static rtdm_irq_t irq_handler_drdy_edge_low;
static u8 active_channel = ADS1256_EMPTY_CHANNEL;

static buffer_t buffer_cmd;
static u8 last_byte_no = 0;
static atomic_t drdy_is_low_status = ATOMIC_INIT(0);
static bool is_blocked_mode = false;
static u32 cycling_size = 1;
static bool is_cycling_mode = false;

module_param(drdy_gpio_pin, short, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(drdy_gpio_pin, "DRDY GPIO pin");
module_param(reset_gpio_pin, short, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(reset_gpio_pin, "RESET GPIO pin");

struct ads1256_rtdm_status_obj {
    struct kobject kobj;
    int status;
};

#define to_ads1256_rtdm_status_obj(x) container_of(x, struct ads1256_rtdm_status_obj, kobj)

struct ads1256_rtdm_status_attribute {
    struct attribute attr;
    ssize_t (*show)(struct ads1256_rtdm_status_obj *foo, struct ads1256_rtdm_status_attribute *attr, char *buf);
    ssize_t (*store)(struct ads1256_rtdm_status_obj *foo, struct ads1256_rtdm_status_attribute *attr, const char *buf, size_t count);
};
#define to_ads1256_rtdm_status_attr(x) container_of(x, struct ads1256_rtdm_status_attribute, attr)



static ssize_t ads1256_rtdm_status_attr_show(struct kobject *kobj,
                                             struct attribute *attr,
                                             char *buf)
{
    struct ads1256_rtdm_status_attribute *attribute;
    struct ads1256_rtdm_status_obj *foo;

    attribute = to_ads1256_rtdm_status_attr(attr);
    foo = to_ads1256_rtdm_status_obj(kobj);

    if (!attribute->show)
        return -EIO;

    return attribute->show(foo, attribute, buf);
}


static ssize_t ads1256_rtdm_status_attr_store(struct kobject *kobj,
                                              struct attribute *attr,
                                              const char *buf, size_t len)
{
    struct ads1256_rtdm_status_attribute *attribute;
    struct ads1256_rtdm_status_obj *foo;

    attribute = to_ads1256_rtdm_status_attr(attr);
    foo = to_ads1256_rtdm_status_obj(kobj);

    if (!attribute->store)
        return -EIO;

    return attribute->store(foo, attribute, buf, len);
}

static struct sysfs_ops ads1256_rtdm_status_sysfs_ops = {
        .show = ads1256_rtdm_status_attr_show,
        .store = ads1256_rtdm_status_attr_store,
};

static void ads1256_rtdm_status_release(struct kobject *kobj)
{
    struct ads1256_rtdm_status_obj *foo;

    foo = to_ads1256_rtdm_status_obj(kobj);
    kfree(foo);
}

static ssize_t ads1256_rtdm_status_show(struct ads1256_rtdm_status_obj *ads1256_rtdm_obj,
                                        struct ads1256_rtdm_status_attribute *attr,
                                        char *buf)
{
    return sprintf(buf, "%d\n", ads1256_rtdm_obj->status);
}

static ssize_t ads1256_rtdm_status_store(struct ads1256_rtdm_status_obj *ads1256_rtdm_obj,
                                         struct ads1256_rtdm_status_attribute *attr,
                                         const char *buf, size_t count)
{
    //sscanf(buf, "%du", &ads1256_rtdm_obj->status);
    return 0;
}

static struct ads1256_rtdm_status_attribute ads1256_rtdm_status_attribute =
        __ATTR(status, 0644, ads1256_rtdm_status_show, ads1256_rtdm_status_store);



static struct attribute *ads1256_rtdm_status_default_attrs[] = {
        &ads1256_rtdm_status_attribute.attr,
        NULL,	/* need to NULL terminate the list of attributes */
};

static struct kobj_type ads1256_rtdm_status_ktype = {
        .sysfs_ops = &ads1256_rtdm_status_sysfs_ops,
        .release = ads1256_rtdm_status_release,
        .default_attrs = ads1256_rtdm_status_default_attrs,
};

static struct kset *ads1256_rtdm_status_kset;
static struct ads1256_rtdm_status_obj *status_obj;


static struct ads1256_rtdm_status_obj *ads1256_rtdm_create_status_obj(const char *name)
{
    struct ads1256_rtdm_status_obj *foo;
    int retval;

    foo = kzalloc(sizeof(*foo), GFP_KERNEL);
    if (!foo)
        return NULL;


    foo->kobj.kset = ads1256_rtdm_status_kset;



    retval = kobject_init_and_add(&foo->kobj, &ads1256_rtdm_status_ktype, NULL, "%s", name);
    if (retval) {
        kobject_put(&foo->kobj);
        return NULL;
    }


    kobject_uevent(&foo->kobj, KOBJ_ADD);

    return foo;
}

static void destroy_ads1256_rtdm_status_obj(struct ads1256_rtdm_status_obj *foo)
{
    kobject_put(&foo->kobj);
}

static void ads1256_rtdm_reset(void) {
    gpio_set_value(reset_gpio_pin, 0);
    udelay(50);
    gpio_set_value(reset_gpio_pin, 1);
}

static void ads1256_rtdm_spi_do_chip_select(void) {
    gpio_set_value(active_cs->cs_gpio, 0);
}

static void ads1256_rtdm_spi_do_chip_deselect(void) {
    gpio_set_value(active_cs->cs_gpio, 1);
}


static int ads1256_rtdm_read_n_bytes(u8 *bytes, u32 n) {
    int ret;
    ret = rtdm_master->ops->read(active_cs, bytes, n);

    return ret;
}

static uint8_t ads1256_rtmd_read_byte(void) {
    u8 byte;
    ssize_t size = 0;
    size = ads1256_rtdm_read_n_bytes(&byte, 1);
    return byte;
}

static ssize_t ads1256_rtdm_write_n_bytes(u8 *bytes, u32 n) {
    ssize_t size = 0;
    size = rtdm_master->ops->write(active_cs, bytes, n);
    return size;
}

static ssize_t ads1256_rtdm_write_byte(uint8_t byte) {
    ssize_t size = 0;
    size = ads1256_rtdm_write_n_bytes(&byte, 1);
    return size;
}


static int ads1256_rtdm_send_cmd(void) {
    ssize_t write_size = 0;


    write_size = ads1256_rtdm_write_n_bytes(&buffer_cmd.data, buffer_cmd.size);
    buffer_cmd.size = 0;
    last_byte_no = 0;
    if (write_size > 0)
        return 0;
    else {

        printk(KERN_ERR "%s: Problem communication with ads1256 using SPI.write_size: %u", __FUNCTION__, write_size);
        return EIO;
    }

}


static int ads1256_rtdm_prepare_cmd(ads1256_commands_e cmd, ads1256_registers_e register_address, uint8_t value) {

    switch (cmd) {
        case ADS1256_COMMAND_RDATA:
        case ADS1256_COMMAND_RDATAC:
        case ADS1256_COMMAND_RESET:
        case ADS1256_COMMAND_SDATAC:
        case ADS1256_COMMAND_SELFCAL:
        case ADS1256_COMMAND_SELFGCAL:
        case ADS1256_COMMAND_SELFOCAL:
        case ADS1256_COMMAND_STNDBY:
        case ADS1256_COMMAND_SYNC:
        case ADS1256_COMMAND_SYSGCAL:
        case ADS1256_COMMAND_SYSOCAL:
        case ADS1256_COMMAND_WAKEUP:
            buffer_cmd.data[last_byte_no++] = cmd;
            buffer_cmd.size++;
            break;

        case ADS1256_COMMAND_RREG:
            switch (register_address) {
                case ADS1256_REGISTER_STATUS:
                case ADS1256_REGISTER_MUX:
                case ADS1256_REGISTER_ADCON:
                case ADS1256_REGISTER_DRATE:
                case ADS1256_REGISTER_IO:
                case ADS1256_REGISTER_OFC0:
                case ADS1256_REGISTER_OFC1:
                case ADS1256_REGISTER_OFC2:
                case ADS1256_REGISTER_FSC0:
                case ADS1256_REGISTER_FSC1:
                case ADS1256_REGISTER_FSC2:
                    buffer_cmd.data[last_byte_no++] = ((uint8_t) cmd + (uint8_t) register_address);
                    buffer_cmd.data[last_byte_no++] = 0x00;
                    buffer_cmd.size += 2;
                    break;
                default:
                    printk(KERN_ERR "%s: Invalid register_address while using ADS1256_COMMAND_RREG or ADS1256_COMMAND_WREG command.", __FUNCTION__);
                    return EINVAL;
            }
            break;
        case ADS1256_COMMAND_WREG:
            switch (register_address) {
                case ADS1256_REGISTER_STATUS:
                case ADS1256_REGISTER_MUX:
                case ADS1256_REGISTER_ADCON:
                case ADS1256_REGISTER_DRATE:
                case ADS1256_REGISTER_IO:
                case ADS1256_REGISTER_OFC0:
                case ADS1256_REGISTER_OFC1:
                case ADS1256_REGISTER_OFC2:
                case ADS1256_REGISTER_FSC0:
                case ADS1256_REGISTER_FSC1:
                case ADS1256_REGISTER_FSC2:
                    buffer_cmd.data[last_byte_no++] = (uint8_t) cmd + (uint8_t) register_address;
                    buffer_cmd.data[last_byte_no++] = 0;
                    buffer_cmd.data[last_byte_no++] = value;
                    buffer_cmd.size += 3;
                    break;
                default:
                    printk(KERN_ERR "%s: Invalid register_address used   while using ADS1256_COMMAND_RREG or ADS1256_COMMAND_WREG command.", __FUNCTION__);
                    return EINVAL;
            }

            break;
        default:
            printk(KERN_ERR "%s: Invalid command.", __FUNCTION__);
            return EINVAL;
    }

    return 0;
}

static int ads1256_rtdm_read_register(ads1256_registers_e register_address, uint8_t *return_value) {
    switch (register_address) {
        case ADS1256_REGISTER_STATUS:
        case ADS1256_REGISTER_MUX:
        case ADS1256_REGISTER_ADCON:
        case ADS1256_REGISTER_DRATE:
        case ADS1256_REGISTER_IO:
        case ADS1256_REGISTER_OFC0:
        case ADS1256_REGISTER_OFC1:
        case ADS1256_REGISTER_OFC2:
        case ADS1256_REGISTER_FSC0:
        case ADS1256_REGISTER_FSC1:
        case ADS1256_REGISTER_FSC2:
            ads1256_rtdm_spi_do_chip_select();
            ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_RREG, register_address, 0);
            ads1256_rtdm_send_cmd();
            udelay(10);
            *return_value = ads1256_rtmd_read_byte();
            ads1256_rtdm_spi_do_chip_deselect();
            return 0;
        default:
            printk(KERN_ERR "%s: Invalid register address used.", __FUNCTION__);
            return EINVAL;

    }
}

static int ads1256_rtdm_read_register_status(ads1256_register_status_t *value) {

    return ads1256_rtdm_read_register(ADS1256_REGISTER_STATUS, &(value->value));
}

static int ads1256_rtdm_read_register_mux(ads1256_register_mux_t *value) {

    return ads1256_rtdm_read_register(ADS1256_REGISTER_MUX, &(value->value));
}

static int ads1256_rtdm_read_register_adcon(ads1256_register_adcon_t *value) {

    return ads1256_rtdm_read_register(ADS1256_REGISTER_ADCON, &(value->value));
}

static int ads1256_rtdm_read_register_io(ads1256_register_io_t *value) {


    return ads1256_rtdm_read_register(ADS1256_REGISTER_IO, &(value->value));
}

static int ads1256_rtdm_read_register_drate(ads1256_register_drate_t *value) {

    return ads1256_rtdm_read_register(ADS1256_REGISTER_DRATE, &(value->value));
}
/*
static int ads1256_rtdm_read_registers_ofc(uint32_t  * value){
    buffer_t buffer;
    ssize_t  write_size;
    value = 0x0;
    buffer.data[0] = (uint8_t) ADS1256_COMMAND_RREG + (uint8_t) ADS1256_REGISTER_OFC0;
    buffer.data[1] = 0x2;
    buffer.size = 2;

    write_size = bcm283x_spi_rtdm_write_rt_using_context(spi_context, &buffer, buffer.size);
    if(write_size <= 0)
    {
        printk(KERN_ERR "Problem communication with ads1256 using SPI.\r\n");
        return EIO;
    }
    if(ads1256_read_data(&buffer) == 0){
        *value = buffer.data[0];
        *value << 16;
        *value += buffer.data[1];
        *value << 8;
        *value += buffer.data[2];
        return 0;
    }else{
        printk(KERN_ERR "Problem communication with ads1256 using SPI.\r\n");
        return EIO;
    }
}*/

/*static int ads1256_rtdm_read_registers_fsc(uint32_t  * value){
    buffer_t buffer;
    ssize_t  write_size;
    value = 0x0;
    buffer.data[0] = (uint8_t) ADS1256_COMMAND_RREG + (uint8_t) ADS1256_REGISTER_FSC0;
    buffer.data[1] = 0x2;
    buffer.size = 2;

    write_size = bcm283x_spi_rtdm_write_rt_using_context(spi_context, &buffer, buffer.size);
    if(write_size <= 0)
    {
        printk(KERN_ERR "Problem communication with ads1256 using SPI.\r\n");
        return EIO;
    }
    if(ads1256_read_data(&buffer) == 0){
        *value = buffer.data[2];
        *value << 16;
        *value += buffer.data[1];
        *value << 8;
        *value += buffer.data[0];
        return 0;
    }else{
        printk(KERN_ERR "Problem communication with ads1256 using SPI.\r\n");
        return EIO;
    }
}*/

static void ads1256_rtdm_print_register_status(void) {
    ads1256_register_status_t register_address;
    register_address.value = 0;
    ads1256_rtdm_read_register_status(&register_address);

    printk(KERN_INFO "[ads1256-rtdm] ADS1256 device status:");
    printk(KERN_INFO "[ads1256-rtdm] ID: %x", register_address.elements.id);
    printk(KERN_INFO "[ads1256-rtdm] ACAL: %x", register_address.elements.acal);
    printk(KERN_INFO "[ads1256-rtdm] BUFEN: %x", register_address.elements.bufen);
    printk(KERN_INFO "[ads1256-rtdm] ORDER: %x", register_address.elements.order);
    printk(KERN_INFO "[ads1256-rtdm] DRDY: %x", register_address.elements.drdy);
}

static void ads1256_rtdm_print_register_data_rate(void) {
    ads1256_register_drate_t register_address;
    ads1256_rtdm_read_register_drate(&register_address);

    printk(KERN_INFO "[ads1256-rtdm] ADS1256 device status:");
    printk(KERN_INFO "[ads1256-rtdm] DATA RATE: %x", register_address.value);

}

static int ads1256_rtdm_open(struct rtdm_fd *fd, int oflags) {
    int device_id;
    ads1256_dev_context_t *context = NULL;
    if (active_channel != ADS1256_EMPTY_CHANNEL && is_cycling_mode == false) {
        return -EBUSY;
    } else {
        printk(KERN_INFO "%s: Device acd0.%d start open.", __FUNCTION__, fd->minor);
        context = (ads1256_dev_context_t *) rtdm_fd_to_private(fd);
        device_id = fd->minor;
        rtdm_lock_get(&global_lock);
        ring_buffer_init(&(context->buffer), ADS1256_BUFFER_SIZE);
        context->analog_input = device_id;
        context->device_used = 1;
        active_channel = device_id;
        ads1256_ads_open_devices[device_id] = context;

        rtdm_lock_put(&global_lock);

        ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_WREG, ADS1256_REGISTER_MUX, (active_channel << 4) + ADS1256_AINCOM);
        ads1256_rtdm_send_cmd();
        udelay(10);

        ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_SYNC, ADS1256_REGISTER_NONE, 0);
        ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_WAKEUP, ADS1256_REGISTER_NONE, 0);
        ads1256_rtdm_send_cmd();
        udelay(10);

        ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_RDATAC, ADS1256_REGISTER_NONE, 0);
        ads1256_rtdm_send_cmd();
        printk(KERN_INFO "%s: Device acd0.%d open.", __FUNCTION__, fd->minor);
        return 0;
    }
}

static void ads1256_rtdm_close(struct rtdm_fd *fd) {
    rtdm_lock_get(&close_lock);
    printk(KERN_INFO "%s: Device acd0.%d start closed.", __FUNCTION__, fd->minor);
    ads1256_dev_context_t *context = NULL;

    context = (ads1256_dev_context_t *) rtdm_fd_to_private(fd);
    ring_buffer_destroy(&context->buffer);
    context->analog_input = fd->minor;
    context->device_used = 0;
    ads1256_ads_open_devices[fd->minor] = NULL;
    active_channel = ADS1256_EMPTY_CHANNEL;
    ads1256_rtdm_reset();
    printk(KERN_INFO "%s: Device acd0.%d closed.", __FUNCTION__, fd->minor);
    rtdm_lock_put(&close_lock);
    return;

}

static ssize_t ads1256_rtdm_read_rt(struct rtdm_fd *fd, void __user *buf, size_t size) {

    u8 * buffer;
    int curr_size;

    ads1256_dev_context_t *context = (ads1256_dev_context_t *) rtdm_fd_to_private(fd);



    curr_size = atomic_long_read(&context->buffer.cur_size);
    printk(KERN_INFO "%s: Device acd0.%d start read.", __FUNCTION__, fd->minor);
    if(is_blocked_mode){
        while(curr_size < size){
            curr_size = atomic_long_read(&context->buffer.cur_size);
            rtdm_task_sleep(1000000);
            printk(KERN_INFO "%s: Device acd0.%d wait read: %d bytes ready.", __FUNCTION__, fd->minor, curr_size);
        }

    }
    if(curr_size > size)
        curr_size = size;

    printk(KERN_INFO "%s: Device acd0.%d read %d bytes.", __FUNCTION__, fd->minor, curr_size);
    buffer = rtdm_malloc(curr_size);
    rtdm_lock_get(&(context->lock));
    ring_buffer_get_n(&(context->buffer), buffer, curr_size);
    rtdm_lock_put(&(context->lock));
    rtdm_safe_copy_to_user(fd, buf, (const void *) buffer, curr_size);
    rtdm_free(buffer);
    printk(KERN_INFO "%s: Device acd0.%d read.", __FUNCTION__, fd->minor);
    return curr_size;

}

static ssize_t ads1256_rtdm_write_rt(struct rtdm_fd *fd, const void __user *buf, size_t size) {
    return 0;
}

static int ads1256_rtdm_set_samplerate(ads1256_data_rate_e sample_rate){

    ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_WREG, ADS1256_REGISTER_DRATE, (u32)sample_rate);
    ads1256_rtdm_send_cmd();
    udelay(10);

    ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_SYNC, ADS1256_REGISTER_NONE, 0);
    ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_WAKEUP, ADS1256_REGISTER_NONE, 0);
    ads1256_rtdm_send_cmd();
    udelay(10);
    return 0;
}

static int ads1256_rtdm_set_pga(ads1256_pga_e pga){

    u8 register_value = 0b00100;
    ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_WREG, ADS1256_REGISTER_ADCON, (register_value << 3) + (u8) pga);
    ads1256_rtdm_send_cmd();
    udelay(10);

    ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_SYNC, ADS1256_REGISTER_NONE, 0);
    ads1256_rtdm_prepare_cmd(ADS1256_COMMAND_WAKEUP, ADS1256_REGISTER_NONE, 0);
    ads1256_rtdm_send_cmd();
    udelay(10);
    return 0;
}

static int ads1256_rtdm_ioctl_rt(struct rtdm_fd *fd, unsigned int request, void __user *arg) {

    int value;
    int res;
    ads1256_data_rate_e sample_rate;
    ads1256_pga_e pga;

    /* Retrieve context */

    switch (request) {

        case ADS1256_SET_SAMPLERATE: /* Change the bit order */

            res = rtdm_safe_copy_from_user(fd, &sample_rate, arg, sizeof(ads1256_data_rate_e));
            if (res) {
                printk(KERN_ERR "%s: Can't retrieve argument from user space (%d)!", __FUNCTION__, res);
                return (res < 0) ? res : -res;
            }

            switch(sample_rate){
                case ADS1256_DATA_RATE_30K_SPS:
                case ADS1256_DATA_RATE_15K_SPS:
                case ADS1256_DATA_RATE_7K5_SPS:
                case ADS1256_DATA_RATE_3K75_SPS:
                case ADS1256_DATA_RATE_2K_SPS:
                case ADS1256_DATA_RATE_1K_SPS:
                case ADS1256_DATA_RATE_0K5_SPS:
                case ADS1256_DATA_RATE_0K1_SPS:
                case ADS1256_DATA_RATE_0K06_SPS:
                case ADS1256_DATA_RATE_0K05_SPS:
                case ADS1256_DATA_RATE_0K03_SPS:
                case ADS1256_DATA_RATE_0K025_SPS:
                case ADS1256_DATA_RATE_0K015_SPS:
                case ADS1256_DATA_RATE_0K01_SPS:
                case ADS1256_DATA_RATE_0K005_SPS:
                case ADS1256_DATA_RATE_0K002_5_SPS:
                    break;
                default:
                    printk(KERN_WARNING "%s: Can't retrieve argument from user space (%d) - wrong value, set default sample rate!", __FUNCTION__, res);
                    sample_rate = ADS1256_DEFAULT_SAMPLE_RATE;
            }

            return ads1256_rtdm_set_samplerate(sample_rate);

        case ADS1256_SET_PGA: /* Change the data mode */
            res = rtdm_safe_copy_from_user(fd, &pga, arg, sizeof(ads1256_pga_e));
            if (res) {
                printk(KERN_ERR "%s: Can't retrieve argument from user space (%d)!", __FUNCTION__, res);
                return (res < 0) ? res : -res;
            }

            switch(pga){
                case ADS1256_PGA1:
                case ADS1256_PGA2:
                case ADS1256_PGA4:
                case ADS1256_PGA8:
                case ADS1256_PGA16:
                case ADS1256_PGA32:
                case ADS1256_PGA64:
                    break;
                default:
                    printk(KERN_WARNING "%s: Can't retrieve argument from user space (%d) - wrong value, set default pga!", __FUNCTION__, res);
                    pga = ADS1256_DEFAULT_PGA;
            }
            return ads1256_rtdm_set_pga(value);

        case ADS1256_SET_CHANNEL_CYCLING_SIZE: /* Change the bus speed */
            res = rtdm_safe_copy_from_user(fd, &value, arg, sizeof(int));
            if (res) {
                printk(KERN_ERR "%s: Can't retrieve argument from user space (%d)!", __FUNCTION__, res);
                return (res < 0) ? res : -res;
            }
            cycling_size = value;
            return 0;


        case ADS1256_SET_BLOCKING_READ: /* Change the chip select polarity */
            res = rtdm_safe_copy_from_user(fd, &value, arg, sizeof(int));
            if (res) {
                printk(KERN_ERR "%s: Can't retrieve argument from user space (%d)!", __FUNCTION__, res);
                return (res < 0) ? res : -res;
            }
            is_blocked_mode = (bool)value;
            return 0;

        default: /* Unexpected case */
            printk(KERN_ERR "%s: Unexpected request : %d!", __FUNCTION__, request);
            return -EINVAL;

    }

}


/**
 * This structure describes the RTDM driver.
 */
static struct rtdm_driver ads1256_rtdm_driver = {
        .profile_info = RTDM_PROFILE_INFO(foo, RTDM_CLASS_EXPERIMENTAL, RTDM_SUBCLASS_GENERIC, 42),
        .device_flags = RTDM_NAMED_DEVICE | RTDM_EXCLUSIVE | RTDM_FIXED_MINOR,
        .device_count = ADS1256_ADC_DEVICES_NUMBER,
        .context_size = sizeof(struct ads1256_dev_context_s),

        .ops = {
                .open = ads1256_rtdm_open,
                .read_rt = ads1256_rtdm_read_rt,
                .read_nrt = ads1256_rtdm_read_rt,
                .write_rt = ads1256_rtdm_write_rt,
                .ioctl_rt = ads1256_rtdm_ioctl_rt,
                .close = ads1256_rtdm_close
        }
};

int ads1256_rtdm_init_ac_devices(void) {

    int device_id, res;
    for (device_id = 0; device_id < ADS1256_ADC_DEVICES_NUMBER; device_id++) {

        /* Set device parameters */
        ads1256_adc_devices[device_id].driver = &ads1256_rtdm_driver;
        ads1256_adc_devices[device_id].label = "acd0.%d";
        ads1256_adc_devices[device_id].minor = device_id;
        ads1256_ads_open_devices[device_id] = NULL;

        /* Try to register the device */
        res = rtdm_dev_register(&ads1256_adc_devices[device_id]);
        if (res == 0) {
            printk(KERN_INFO "%s: Device acd0.%d registered without errors.", __FUNCTION__, device_id);
        } else {
            printk(KERN_ERR "%s: Device acd0.%d registration failed : ", __FUNCTION__, device_id);
            switch (res) {
                case -EINVAL:
                    printk(KERN_ERR "%s: The descriptor contains invalid entries.",__FUNCTION__);
                    break;

                case -EEXIST:
                    printk(KERN_ERR "%s: The specified device name of protocol ID is already in use.",__FUNCTION__);
                    break;

                case -ENOMEM:
                    printk(KERN_ERR "%s: A memory allocation failed in the process of registering the device.", __FUNCTION__);
                    break;

                default:
                    printk(KERN_ERR "%s: Unknown error code returned.", __FUNCTION__);
                    break;
            }
            return res;
        }
    }
    return 0;
}

static int ads1256_rtdm_drdy_is_low(rtdm_irq_t *irqh) {
    atomic_set(&drdy_is_low_status, 1);

    return RTDM_IRQ_HANDLED;
}


static void ads1256_rtdm_search_device_on_spi(void *arg) {
    int *return_value = (int)arg;
    int irq_trigger = 0, ret;
    unsigned int irq;
    struct list_head *i = NULL;
    struct rtdm_spi_config *config = NULL;
    struct spi_master *master;
    cpumask_t mask;
    ads1256_register_status_t register_address;

    master = spi_busnum_to_master(0);
    rtdm_master = dev_get_drvdata(&master->dev);
    if (rtdm_master == NULL) {
        printk(KERN_DEBUG "%s: MASTER not found.", __FUNCTION__);
        return;
    } else {
        printk(KERN_DEBUG "%s: MASTER found.",__FUNCTION__);

    }


    list_for_each(i, &rtdm_master->slaves) {
        active_cs = list_entry(i, struct rtdm_spi_remote_slave, next);
        ads1256_rtdm_spi_do_chip_select();

        config = &active_cs->config;
        config->bits_per_word = 8;
        config->mode = SPI_MODE_1;
        config->speed_hz = 3920000;
        rtdm_master->ops->configure(active_cs);


        printk(KERN_INFO "%s: Search ADS1256 device on %s",__FUNCTION__, active_cs->dev.name);



        register_address.value = 0;
        ads1256_rtdm_read_register_status(&register_address);
        if (register_address.elements.id == ADS1256_DEVICE_ID) {
            ads1256_rtdm_spi_do_chip_select();

            irq = gpio_to_irq(drdy_gpio_pin);
            irq_trigger |= IRQ_TYPE_EDGE_FALLING;

            if (irq_trigger)
                irq_set_irq_type(irq, irq_trigger);


            ret = rtdm_irq_request(&irq_handler_drdy_edge_low, irq, ads1256_rtdm_drdy_is_low, RTDM_IRQTYPE_EDGE, "DRDY",
                                   NULL);
            printk(KERN_ERR "%s: RTDM IRQ register results: %d %u",__FUNCTION__, ret, irq);


            cpumask_set_cpu(3, &mask);

            xnintr_affinity(&irq_handler_drdy_edge_low, mask);

            printk(KERN_INFO "%s: Found ADS1256 device on %s", __FUNCTION__, active_cs->dev.name);
            *return_value = 0;

            return;
        }
        ads1256_rtdm_spi_do_chip_deselect();
    }


    printk(KERN_ERR "%s: Device ADS1256 not connected.", __FUNCTION__);
    *return_value = -ENOENT;
    return;
}

int ads1256_rtdm_task_init(rtdm_task_t *task, const char *name,
                           rtdm_task_proc_t task_proc, void *arg,
                           int priority, nanosecs_rel_t period) {
    union xnsched_policy_param param;
    struct xnthread_start_attr sattr;
    struct xnthread_init_attr iattr;
    int err;
    cpumask_t mask;

    iattr.name = name;
    iattr.flags = 0;
    iattr.personality = &xenomai_personality;

    cpumask_set_cpu(3, &mask);

    iattr.affinity = mask;
    param.rt.prio = priority;

    err = xnthread_init(task, &iattr, &xnsched_class_rt, &param);
    if (err)
        return err;

    /* We need an anonymous registry entry to obtain a handle for fast
       mutex locking. */
    err = xnthread_register(task, "");
    if (err)
        goto cleanup_out;

    if (period > 0) {
        err = xnthread_set_periodic(task, XN_INFINITE,
                                    XN_RELATIVE, period);
        if (err)
            goto cleanup_out;
    }

    sattr.mode = 0;
    sattr.entry = task_proc;
    sattr.cookie = arg;
    err = xnthread_start(task, &sattr);
    if (err)
        goto cleanup_out;

    return 0;

    cleanup_out:
    xnthread_cancel(task);
    return err;
}


static void ads1256_rtdm_get_data_from_device(void *arg) {
    // ktime_t start;

    int i = 0;
    uint8_t data[3];
    atomic_set(&drdy_is_low_status, 1);
    //start = ktime_get();
    while (!rtdm_task_should_stop()) {
        rtdm_lock_get(&close_lock);
        /* if ((ktime_get().tv64 - start.tv64) > 1000000000) {
             start = ktime_get();
             rtdm_printk(KERN_ERR
                                 "Sample rate: %d", i);
             i = 0;
         }*/
        if(active_channel != ADS1256_EMPTY_CHANNEL) {
            if (is_cycling_mode == false) {
                if (atomic_read(&drdy_is_low_status) == 1) {
                    atomic_set(&drdy_is_low_status, 0);
                    ads1256_rtdm_read_n_bytes(data, 3);
                    rtdm_lock_get(&(ads1256_ads_open_devices[active_channel]->lock));
                    ring_buffer_npush_back(&(ads1256_ads_open_devices[active_channel]->buffer), data, 3);
                    rtdm_lock_put(&(ads1256_ads_open_devices[active_channel]->lock));
                    //i++;
                }
            }
        }else{
            rtdm_task_sleep(1e8);
        }
        rtdm_lock_put(&close_lock);
    }
}

static void ads1256_rtdm_unregister_devices(void) {
    int device_id;
    for (device_id = 0; device_id < ADS1256_ADC_DEVICES_NUMBER; device_id++) {
        if (ads1256_ads_open_devices[device_id] != NULL) {
            ads1256_ads_open_devices[device_id]->device_used = 0;
            ring_buffer_destroy(&(ads1256_ads_open_devices[device_id]->buffer));
            ads1256_ads_open_devices[device_id] = NULL;
        }
        printk(KERN_INFO "%s: Unregistering device acd0.%d  ...\r\n", __FUNCTION__, device_id);
        rtdm_dev_unregister(&ads1256_adc_devices[device_id]);
        printk(KERN_INFO "%s: Device acd0.%d unregistered  ...\r\n", __FUNCTION__, device_id);
    }
}

static void ads1256_rtdm_prepare_gpio(void) {
    gpio_request_one(drdy_gpio_pin, GPIOF_IN, "DRDY");
    gpio_request_one(reset_gpio_pin, GPIOF_OUT_INIT_HIGH, "RESET");
}




static int __init ads1256_rtdm_init(void) {
    int res = -1;
    buffer_cmd.size = 0;

    printk(KERN_INFO "%s: Real-Time A/C driver for the ADS1256 init!", __FUNCTION__);

    ads1256_rtdm_status_kset = kset_create_and_add("ads1256_rtdm", NULL, kernel_kobj);
    if (!ads1256_rtdm_status_kset)
        return -ENOMEM;

    status_obj = ads1256_rtdm_create_status_obj("status");
    if (!status_obj)
        destroy_ads1256_rtdm_status_obj(status_obj);

    status_obj->status=0;



    ads1256_rtdm_init_ac_devices();
    ads1256_rtdm_prepare_gpio();
    printk(KERN_DEBUG "%s: Create sysfs status kobject", __FUNCTION__);

    rtdm_task_init(&ads1256_search_ac_task, "search_device_on_spi", ads1256_rtdm_search_device_on_spi, &res, 99, 20000);
    rtdm_task_join(&ads1256_search_ac_task);
    if (res == 0) {
        ads1256_rtdm_task_init(&ads1256_task_agent, "rtdm_agent", ads1256_rtdm_get_data_from_device, NULL, 99, 0);


        status_obj->status = 1;
        return 0;
    } else {
        ads1256_rtdm_unregister_devices();
        return 0; /**To do fix value**/
    }


}

static void ads1256_rtdm_free_gpios(void) {
    gpio_free(drdy_gpio_pin);
    gpio_free(reset_gpio_pin);
}

static void __exit ads1256_rtdm_exit(void) {
    ads1256_rtdm_reset();
    ads1256_rtdm_free_gpios();
    rtdm_irq_free(&irq_handler_drdy_edge_low);
    ads1256_rtdm_spi_do_chip_deselect();
    destroy_ads1256_rtdm_status_obj(status_obj);
    kset_unregister(ads1256_rtdm_status_kset);

    if (status_obj->status == 1) {
        rtdm_task_destroy(&ads1256_task_agent);
        ads1256_rtdm_unregister_devices();
    }

    printk(KERN_INFO "%s Cleaning up module.", __FUNCTION__);
}

module_init(ads1256_rtdm_init);
module_exit(ads1256_rtdm_exit);


/*
 * Register module values
 */
#ifndef GIT_VERSION
#define GIT_VERSION "0.1-untracked";
#endif /* ! GIT_VERSION */
MODULE_VERSION(GIT_VERSION);
MODULE_DESCRIPTION("Real-Time A/C driver for the ads1256 using the RTDM API");
MODULE_AUTHOR("Piotr Piórkowski <qba100@gmail.com>");
MODULE_LICENSE("GPL");
