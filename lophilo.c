/*
 * Demonstration of the use of debugfs with Lophilo
 *
 * Author: Ricky Ng-Adam <rngadam@lophilo.com>
 *         Zhizhou Li <lzz@meteroi.com>
 *

 Copyright 2012 Lophilo

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/ioport.h>
#include <asm/io.h>
#include <asm/uaccess.h>  /* for put_user */
#include <linux/mm.h> // remap_pfn_range
#include <asm/page.h> // page_to_pfn
#include <linux/slab.h>   /* kmalloc() */
#include <linux/interrupt.h>
#include <linux/spinlock.h>//for use spinlock
#include <linux/sched.h>
#include <linux/wait.h>
#include <mach/gpio.h>
#include <linux/of_irq.h>


// from linux/arch/arm/mach-at91/board-tabby.c
extern void __iomem *fpga_cs0_base;
extern void __iomem *fpga_cs1_base;
extern void __iomem *fpga_cs2_base;
extern void __iomem *fpga_cs3_base;

#define MAX_SUBSYSTEMS 32
#define MAX_REGISTRY_SIZE PAGE_SIZE*4
#define MAX_PARENT_NAME 32
#define MAX_IO_NAME 5 // ioXX\0

#define GPIO_SUBSYSTEM 0xea680001
#define PWM_SUBSYSTEM 0xea680002

#define SYS_PHYS_ADDR 0x10000000
#define MOD_PHYS_ADDR 0x20000000

#define SYS_SUBSYSTEM_ID       0
#define MOD_SUBSYSTEM_ID       1
#define FPGA_DATA_ID           2
#define FPGA_DOWNLOAD_ID       3
#define IRQ_0_ID               4
#define IRQ_1_ID               5
#define IRQ_2_ID               6
#define IRQ_3_ID               7
#define IRQ_4_ID               8
#define IRQ_5_ID               9
#define IRQ_6_ID               10
#define IRQ_7_ID               11

#define FPGA_DOWNLOAD_BUFFER_SIZE 500*1024

static char* fpga_download_buffer   = NULL;
static int   fpga_buffer_index      = 0;

struct subsystem {
	u32 id;
	u32 size;
	u32 vaddr;
	u32 offset;
	u32 index;
	u32 paddr;
	u8 opened;
};

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);
static int map_lophilo(struct file *filp, struct vm_area_struct *vma);

struct file_operations fops_mem = {
	.owner   = THIS_MODULE,
	.read = device_read,
	.write = device_write,
	.open = device_open,
	.release = device_release,
	.mmap    = map_lophilo
 };

static struct subsystem subsystems[MAX_SUBSYSTEMS];

static struct subsystem sys_subsystem = {
	.id = SYS_SUBSYSTEM_ID,
	.size = 0x204,
	.paddr = SYS_PHYS_ADDR,
	.offset = 0
};

static struct subsystem mod_subsystem = {
	.id = MOD_SUBSYSTEM_ID,
	.paddr = MOD_PHYS_ADDR,
	.offset = 0
};

static struct subsystem fpga_data = {
    .id = FPGA_DATA_ID,
};

static struct subsystem fpga_download = {
    .id = FPGA_DOWNLOAD_ID,
};

static struct subsystem irq0 = {
    .id = IRQ_0_ID,
};

static struct subsystem irq1 = {
    .id = IRQ_1_ID,
};

static struct subsystem irq2 = {
    .id = IRQ_2_ID,
};

static struct subsystem irq3 = {
    .id = IRQ_3_ID,
};

static struct subsystem irq4 = {
    .id = IRQ_4_ID,
};

static struct subsystem irq5 = {
    .id = IRQ_5_ID,
};

static struct subsystem irq6 = {
    .id = IRQ_6_ID,
};

static struct subsystem irq7 = {
    .id = IRQ_7_ID,
};

char registry[MAX_REGISTRY_SIZE];
static struct debugfs_blob_wrapper registry_blob = {
	.data = registry,
	.size = 0
};

struct resource * fpga;

static struct dentry *lophilo_dentry;
static struct dentry *fpga_dentry;

char parent_name[MAX_PARENT_NAME]; // for generating names

#define CREATE_CHANNEL_FILE(size, name, offset) \
	debugfs_create_x##size( \
		name, \
		S_IRWXU | S_IRWXG | S_IRWXO, \
		root, \
		(void*) addr + offset); \
	create_registry_entry(size, parent_name, name, addr, offset);

void create_registry_entry(u8 size, char* parent_name, char* name, u32 addr, u32 offset)
{
	char* type_sys = "sys";
	char* type_mod = "mod";
	char* type;
	int length;

	if(addr < (u32)fpga_cs1_base) {
		type = type_sys;
		offset += addr - (u32)fpga_cs0_base;
	}  else {
		type = type_mod;
		offset += addr - (u32)fpga_cs1_base;
	}
	// size is number of characters written, excluding trailing '\0'
	if(registry_blob.size+1 >= MAX_REGISTRY_SIZE) {
		printk(KERN_ERR "Unable to add %s/%s to registry; out of space", parent_name, name);
		return;
	}
	// http://www.kernel.org/doc/htmldocs/kernel-api/API-scnprintf.html
	// The return value is the number of characters written into buf not including the trailing '\0'.
	// If size is == 0 the function returns 0.
	length = scnprintf(&registry[registry_blob.size],
		MAX_REGISTRY_SIZE - registry_blob.size,
		"%s %u %s %s %u\n",
		type, size, parent_name, name, offset);
	registry_blob.size += length;
	//printk(KERN_INFO "registry updated size: %d", registry_blob.size);
}


 struct dentry * create_channel_gpio(u8 id, struct dentry * parent, u32 addr)
 {
 	int i;
 	struct dentry * root;
 	char io_name[MAX_IO_NAME];

 	scnprintf(parent_name, MAX_PARENT_NAME, "gpio%d", id);
 	root  = debugfs_create_dir(parent_name, parent);

 	CREATE_CHANNEL_FILE(32, "dout", 0x8);
 	CREATE_CHANNEL_FILE(32, "din", 0xc);
 	CREATE_CHANNEL_FILE(32, "doe", 0x10);
 	CREATE_CHANNEL_FILE(32, "imask", 0x20);
 	CREATE_CHANNEL_FILE(32, "iclr", 0x24);
 	CREATE_CHANNEL_FILE(32, "ie", 0x28);
 	CREATE_CHANNEL_FILE(32, "iinv", 0x2c);
 	CREATE_CHANNEL_FILE(32, "iedge", 0x30);
 	for(i=0; i<26; i++) {
 		scnprintf(io_name, MAX_IO_NAME, "io%d", i);
 		CREATE_CHANNEL_FILE(8, io_name, 0x40 + i);
 	}
 	return root;
 }

 struct dentry * create_led(u8 id,  struct dentry * parent, u32 addr)
 {
 	struct dentry * root;

	scnprintf(parent_name, MAX_PARENT_NAME, "led%d", id);
	root = debugfs_create_dir(parent_name, parent);

 	CREATE_CHANNEL_FILE(8, "b",  0x100 + 0x4 * id);
 	CREATE_CHANNEL_FILE(8, "g",  0x101 + 0x4 * id);
 	CREATE_CHANNEL_FILE(8, "r",  0x102 + 0x4 * id);
 	CREATE_CHANNEL_FILE(8, "s",  0x103 + 0x4 * id);
 	CREATE_CHANNEL_FILE(32, "srgb",  0x100 + 0x4 * id);

 	return root;
 }

 struct dentry * create_channel_pwm(u8 id, struct dentry * parent, u32 addr)
 {
 	struct dentry * root;

 	scnprintf(parent_name, MAX_PARENT_NAME, "pwm%d", id);
 	root = debugfs_create_dir(parent_name, parent);

 	CREATE_CHANNEL_FILE(8, "reset", 0x8);
 	CREATE_CHANNEL_FILE(8, "outinv", 0x9);
 	CREATE_CHANNEL_FILE(8, "pmen", 0xa);
 	CREATE_CHANNEL_FILE(8, "fmen", 0xb);
 	CREATE_CHANNEL_FILE(32, "gate", 0xc);
 	CREATE_CHANNEL_FILE(32, "dtyc", 0x10);

	return root;
 }

 void create_root(struct dentry * root, u32 addr)
 {
 	strcpy(parent_name, "lophilo"); // strlen(lophilo) << MAX_PARENT_NAME
 	CREATE_CHANNEL_FILE(16, "id", 0x0);
	CREATE_CHANNEL_FILE(16, "flag", 0x2);
	CREATE_CHANNEL_FILE(32, "ver", 0x4);
	CREATE_CHANNEL_FILE(32, "lock", 0x8);
	CREATE_CHANNEL_FILE(32, "lockb", 0xc);
	CREATE_CHANNEL_FILE(32, "power", 0x200);
}


static void GRID_RESET(void)
{
    at91_set_gpio_value(AT91_PIN_PA27, 0);
}
static GRID_UNRESET(void)
{
    at91_set_gpio_value(AT91_PIN_PA27, 1);
}

static void FPGA_CONF_N(void)
{
    at91_set_gpio_value(AT91_PIN_PB16, 0);
}
static void FPGA_CONF_P(void)
{
    at91_set_gpio_value(AT91_PIN_PB16, 1);
}
static void FPGA_DCLK_N(void)
{
    at91_set_gpio_value(AT91_PIN_PB17, 0);
}
static void FPGA_DCLK_P(void)
{
    at91_set_gpio_value(AT91_PIN_PB17, 1);
}
static void FPGA_DATA_N(void)
{
    at91_set_gpio_value(AT91_PIN_PB15, 0);
}
static void FPGA_DATA_P(void)
{
    at91_set_gpio_value(AT91_PIN_PB15, 1);
}
static int FPGA_DONE(void)
{
    return at91_get_gpio_value(AT91_PIN_PB14);
}
static int FPGA_STAT(void)
{
    return at91_get_gpio_value(AT91_PIN_PB18);
}
static void SYS_RESET(void)
{
    at91_set_gpio_value(AT91_PIN_PB29, 0);
}
static void SYS_UNRESET(void)
{
    at91_set_gpio_value(AT91_PIN_PB29, 1);
}
static void BTNDIS_N(void)
{
    at91_set_gpio_value(AT91_PIN_PB2, 0);
}
static void BTNDIS_P(void)
{
    at91_set_gpio_value(AT91_PIN_PB2, 1);
}

void FPGA_Config(unsigned char* gridFilebuffer, int gridFileSize)
{
    int i;
    unsigned char buf, cnt;

    at91_set_GPIO_periph(AT91_PIN_PA27,0);
    if(at91_set_gpio_output(AT91_PIN_PA27, 0)) {
        printk(KERN_DEBUG"Could not set pin %i for GPIO input.\n", AT91_PIN_PD10);
    }
    GRID_RESET();

    at91_set_GPIO_periph(AT91_PIN_PB18,0);
    at91_set_GPIO_periph(AT91_PIN_PB17,0);
    at91_set_GPIO_periph(AT91_PIN_PB16,0);
    at91_set_GPIO_periph(AT91_PIN_PB15,0);
    at91_set_GPIO_periph(AT91_PIN_PB14,0);

    if(at91_set_gpio_output(AT91_PIN_PB17, 0)) {
        printk(KERN_DEBUG"Could not set pin %i for GPIO input.\n", AT91_PIN_PB17);
    }
    if(at91_set_gpio_output(AT91_PIN_PB16, 0)) {
        printk(KERN_DEBUG"Could not set pin %i for GPIO input.\n", AT91_PIN_PB16);
    }
    if(at91_set_gpio_output(AT91_PIN_PB15, 0)) {
        printk(KERN_DEBUG"Could not set pin %i for GPIO input.\n", AT91_PIN_PB15);
    }
	if(at91_set_gpio_input(AT91_PIN_PB18, 0)) {
		printk(KERN_DEBUG"Could not set pin %i for GPIO input.\n", AT91_PIN_PB18);
	}
	if(at91_set_gpio_input(AT91_PIN_PB14, 0)) {
		printk(KERN_DEBUG"Could not set pin %i for GPIO input.\n", AT91_PIN_PB14);
	}

    FPGA_CONF_N();

    FPGA_CONF_P();

    while(!FPGA_STAT());

    printk("Start config FPGA [");

    for(i = 0; i <= gridFileSize; i++)
    {
        buf = *(gridFilebuffer + i);

        for(cnt = 0; cnt < 8; cnt++)
        {
            if(((buf>>(cnt))&(0x1))==0x1)
            {
                FPGA_DATA_P();
            }
            else
            {
                FPGA_DATA_N();
            }
            FPGA_DCLK_P();
            FPGA_DCLK_N();
        }

        if(FPGA_DONE())
        {
            printk("]\n\r");
            break;
        }

        if(i % 12000 == 0) printk(".");
    }

    if(!FPGA_DONE()) printk("FPGA configuration failed.\n");
    else {
        GRID_UNRESET();
    }
}

DECLARE_WAIT_QUEUE_HEAD (EINT0);
volatile unsigned char irq_flag = 0;
static irqreturn_t irq_interrupt_irq0(int irq, void *dev_id, struct pt_regs *regs)
{
   printk("<1>interrupt 0\n");
   irq_flag = 1;
   wake_up_interruptible(&EINT0);
   return IRQ_HANDLED;
}

static int __init
lophilo_init(void)
{
	struct dentry *lophilo_subsystem_dentry;
	int subsystem_id = 0;
	int result;

	void* current_addr;
	int i, ret;
	u8 pwm_id = 0;
	u8 gpio_id = 0;

	printk(KERN_INFO "Lophilo module loading\n");

	lophilo_dentry = debugfs_create_dir(
		"lophilo",
		NULL);
	if(lophilo_dentry == NULL) {
		printk(KERN_ERR "Could not create root directory entry lophilo in debugfs");
		return -EINVAL;
	}

    fpga_dentry = debugfs_create_dir(
        "fpga",
        NULL);
    if(fpga_dentry == NULL) {
        printk(KERN_ERR "Could not create root directory entry FPGA in debugfs");
        return -EINVAL;
    }

    /** Set pin as GPIO periph, without internal pull up */
    at91_set_GPIO_periph(AT91_PIN_PD10,0);   //M1-EINT0
    at91_set_GPIO_periph(AT91_PIN_PD11,0);   //M1-EINT1
    at91_set_GPIO_periph(AT91_PIN_PD13,0);   //M1-EINT2
    at91_set_GPIO_periph(AT91_PIN_PD14,0);   //M1-EINT3
    at91_set_GPIO_periph(AT91_PIN_PD17,0);   //M1-EINT4
    at91_set_GPIO_periph(AT91_PIN_PD18,0);   //M1-EINT5
    at91_set_GPIO_periph(AT91_PIN_PD19,0);   //M1-EINT6
    at91_set_GPIO_periph(AT91_PIN_PB0,0);    //M1-EINT7

    /** Set pin as GPIO input, without internal pull up */
    if(at91_set_gpio_input(AT91_PIN_PD10, 0)) {
        printk(KERN_ERR"Could not set pin %i for GPIO input.\n", AT91_PIN_PD10);
    }
    if(at91_set_gpio_input(AT91_PIN_PD11, 0)) {
        printk(KERN_ERR"Could not set pin %i for GPIO input.\n", AT91_PIN_PD11);
    }
    if(at91_set_gpio_input(AT91_PIN_PD13, 0)) {
        printk(KERN_ERR"Could not set pin %i for GPIO input.\n", AT91_PIN_PD13);
    }
    if(at91_set_gpio_input(AT91_PIN_PD14, 0)) {
        printk(KERN_ERR"Could not set pin %i for GPIO input.\n", AT91_PIN_PD14);
    }
    if(at91_set_gpio_input(AT91_PIN_PD17, 0)) {
        printk(KERN_ERR"Could not set pin %i for GPIO input.\n", AT91_PIN_PD17);
    }
    if(at91_set_gpio_input(AT91_PIN_PD18, 0)) {
        printk(KERN_ERR"Could not set pin %i for GPIO input.\n", AT91_PIN_PD18);
    }
    if(at91_set_gpio_input(AT91_PIN_PD19, 0)) {
        printk(KERN_ERR"Could not set pin %i for GPIO input.\n", AT91_PIN_PD19);
    }
    if(at91_set_gpio_input(AT91_PIN_PB0, 0)) {
        printk(KERN_ERR"Could not set pin %i for GPIO input.\n", AT91_PIN_PB0);
    }

    /** Set deglitch for pin */
    if(at91_set_deglitch(AT91_PIN_PD10, 1)) {
        printk(KERN_ERR"Could not set pin %i for GPIO deglitch.\n", AT91_PIN_PD10);
    }
    if(at91_set_deglitch(AT91_PIN_PD11, 1)) {
        printk(KERN_ERR"Could not set pin %i for GPIO deglitch.\n", AT91_PIN_PD11);
    }
    if(at91_set_deglitch(AT91_PIN_PD13, 1)) {
        printk(KERN_ERR"Could not set pin %i for GPIO deglitch.\n", AT91_PIN_PD13);
    }
    if(at91_set_deglitch(AT91_PIN_PD14, 1)) {
        printk(KERN_ERR"Could not set pin %i for GPIO deglitch.\n", AT91_PIN_PD14);
    }
    if(at91_set_deglitch(AT91_PIN_PD18, 1)) {
        printk(KERN_ERR"Could not set pin %i for GPIO deglitch.\n", AT91_PIN_PD18);
    }
    if(at91_set_deglitch(AT91_PIN_PD19, 1)) {
        printk(KERN_ERR"Could not set pin %i for GPIO deglitch.\n", AT91_PIN_PD19);
    }
    if(at91_set_deglitch(AT91_PIN_PB0, 1)) {
        printk(KERN_ERR"Could not set pin %i for GPIO deglitch.\n", AT91_PIN_PB0);
    }

    /** Request IRQ for pin */
    if(ret = request_irq(AT91_PIN_PD10, irq_interrupt_irq0, IRQ_TYPE_EDGE_BOTH, "irq_interrupt", NULL))  {
        printk(KERN_ERR"Can't register IRQ %d, mode %d\n", AT91_PIN_PD10, IRQ_TYPE_EDGE_BOTH);
        printk(KERN_ERR"ret = %d\n", ret);
        //return -EIO;
    }

    printk(KERN_ERR "lizhizhou 2");

    debugfs_create_file(
        "data",
        S_IRWXU | S_IRWXG | S_IRWXO,
        fpga_dentry,
        &fpga_data,
        &fops_mem
        );
    debugfs_create_file(
        "download",
        S_IRWXU | S_IRWXG | S_IRWXO,
        fpga_dentry,
        &fpga_download,
        &fops_mem
        );
	debugfs_create_file(
		"EINT0",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&irq0,
		&fops_mem
		);
	debugfs_create_file(
		"EINT1",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&irq1,
		&fops_mem
		);
	debugfs_create_file(
		"EINT2",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&irq2,
		&fops_mem
		);
	debugfs_create_file(
		"EINT3",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&irq3,
		&fops_mem
		);
	debugfs_create_file(
		"EINT4",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&irq4,
		&fops_mem
		);
	debugfs_create_file(
		"EINT5",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&irq5,
		&fops_mem
		);
	debugfs_create_file(
		"EINT6",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&irq6,
		&fops_mem
		);
	debugfs_create_file(
		"EINT7",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&irq7,
		&fops_mem
		);

	//fpga = request_mem_region(FPGA_BASE_ADDR, SIZE16MB, "Lophilo FPGA LEDs");
	sys_subsystem.vaddr = (u32) fpga_cs0_base;
	mod_subsystem.vaddr = (u32) fpga_cs1_base;

	create_root(lophilo_dentry, sys_subsystem.vaddr);

	debugfs_create_file(
		"sysmem",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&sys_subsystem,
		&fops_mem
		);

	debugfs_create_file(
		"modmem",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&mod_subsystem,
		&fops_mem
		);


	for(i=0; i<4; i++) {
		create_led(i, lophilo_dentry, sys_subsystem.vaddr);
	}

	current_addr = fpga_cs1_base;

	while(true) {
		if(subsystem_id == MAX_SUBSYSTEMS) {
			printk(KERN_INFO "Lophilo ended detection, maximum found %d\n",
				MAX_SUBSYSTEMS);
			break;
		}

		subsystems[subsystem_id].id = ioread32(current_addr + 0x4);
		subsystems[subsystem_id].offset = current_addr - fpga_cs1_base;

		if((subsystems[subsystem_id].id & 0xea000000) == 0xea000000) {
			printk(KERN_INFO "Lophilo adding subsystem 0x%x of type 0x%x at 0x%x\n",
				subsystem_id, subsystems[subsystem_id].id, (u32)current_addr);
		} else {
			printk(KERN_INFO "Lophilo ended detection, found 0x%x\n",
				subsystems[subsystem_id].id);
			break;
		}

		subsystems[subsystem_id].vaddr = (u32) current_addr;
		subsystems[subsystem_id].paddr = MOD_PHYS_ADDR;

		subsystems[subsystem_id].size = ioread32(current_addr);
		if(subsystems[subsystem_id].size < 4) {
			printk(KERN_ERR "Invalid subsystem size %d, aborting detection\n",
			       subsystems[subsystem_id].size);
			break;
		}

		switch(subsystems[subsystem_id].id) {
			case GPIO_SUBSYSTEM:
				lophilo_subsystem_dentry = create_channel_gpio(
					gpio_id++,
					lophilo_dentry,
					subsystems[subsystem_id].vaddr);
				break;
			case PWM_SUBSYSTEM:
				lophilo_subsystem_dentry = create_channel_pwm(
					pwm_id++,
					lophilo_dentry,
					subsystems[subsystem_id].vaddr);
				break;
			default:
				printk(KERN_ERR "Unsupported file system id %d", subsystems[subsystem_id].id);
				return -EINVAL;

		}

		debugfs_create_x32(
			"size",
			S_IRWXU | S_IRWXG | S_IRWXO,
			lophilo_subsystem_dentry,
			&subsystems[subsystem_id].size);

		debugfs_create_x32(
			"id",
			S_IRWXU | S_IRWXG | S_IRWXO,
			lophilo_subsystem_dentry,
			&subsystems[subsystem_id].id);

		debugfs_create_x32(
			"addr",
			S_IRWXU | S_IRWXG | S_IRWXO,
			lophilo_subsystem_dentry,
			&subsystems[subsystem_id].vaddr);


		current_addr += subsystems[subsystem_id].size;
		mod_subsystem.size += subsystems[subsystem_id].size;
		//printk(KERN_INFO "current_addr increment to 0x%x for subsystem 0x%x", current_addr, subsystem_id);
		subsystem_id++;
	}

	debugfs_create_blob(
		"registry",
		S_IRWXU | S_IRWXG | S_IRWXO,
		lophilo_dentry,
		&registry_blob);
	return 0;
}

static int
map_lophilo(struct file *filp, struct vm_area_struct *vma)
{
	long unsigned int size = vma->vm_end - vma->vm_start;
	long unsigned int target_size = PAGE_SIZE;
	struct subsystem* subsystem_ptr = (struct subsystem*) filp->private_data;


	if(size != target_size) {
		printk(KERN_INFO "Invalid allocation request, expected %ld, got %ld",
			target_size, size);
                return -EAGAIN;
	}

	/*

	Comment adapted from:

	http://fixunix.com/kernel/242682-mapping-pci-memory-user-space.html

	There is no relationship between the address returned from ioremap and
	what you pass into io_remap_page_range(). ioremap gives you a kernel
	virtual address for the hardware address you remap. io_remap_page_range()
	creates a userspace mapping in the same way, and you should pass in
	the hw address exactly the same way you pass in the hw address into
	ioremap. io_remap_pfn_range() takes a PFN ("page frame number"),
	which is basically the hw address you want to map divided by
	PAGE_SIZE. The main reason for using PFNs is that they allow you to
	map addresses above 4G even if sizeof long is only 4.
	*/
	if (remap_pfn_range(
			vma,
			vma->vm_start,
			subsystem_ptr->paddr >> PAGE_SHIFT,
			PAGE_SIZE,
			vma->vm_page_prot)) {
		printk(KERN_INFO "Allocation failed!");
                return -EAGAIN;
	}

	return 0;
}

void __exit
lophilo_cleanup(void)
{
	printk(KERN_INFO "Lophilo module uninstalling\n");
	debugfs_remove_recursive(lophilo_dentry);
	debugfs_remove_recursive(fpga_dentry);
	//release_mem_region(FPGA_BASE_ADDR, SIZE16MB);
    free_irq(AT91_PIN_PD10, irq_interrupt_irq0);
	return;
}

/* Methods */
/* Called when a process tries to open the device file, like
 * "cat /dev/mycharfile"
 */
static int device_open(struct inode *inode, struct file *file)
{
   struct subsystem* subsystem_ptr = inode->i_private;

   if (subsystem_ptr->opened)
   	return -EBUSY;
   subsystem_ptr->opened++;

   subsystem_ptr->index = 0;
   file->private_data = subsystem_ptr;

   return 0;
}

/* Called when a process closes the device file */
static int device_release(struct inode *inode, struct file *file)
{
   struct subsystem* subsystem_ptr = inode->i_private;

   subsystem_ptr->opened --;     /* We're now ready for our next caller */

   return 0;
}

/* Called when a process, which already opened the dev file, attempts to
   read from it.
*/
static ssize_t device_read(struct file *filp,
   char *buffer,    /* The buffer to fill with data */
   size_t length,   /* The length of the buffer     */
   loff_t *offset)  /* Our offset in the file       */
{
   struct subsystem* subsystem_ptr = filp->private_data;

   /* Number of bytes actually written to the buffer */
   int bytes_read = 0;

   if(subsystem_ptr->index >= subsystem_ptr->size)
   	return 0;
   switch (subsystem_ptr->id)
   {
	   case IRQ_0_ID:
		   interruptible_sleep_on(&EINT0);
		   printk("Read irq 0\n");
		   break;
	   default:
		   /* Actually put the data into the buffer */
		   while (length && (subsystem_ptr->index < subsystem_ptr->size))  {

				/* The buffer is in the user data segment, not the kernel segment;
				 * assignment won't work.  We have to use put_user which copies data from
				 * the kernel data segment to the user data segment. */
				 put_user(
					*((char*) (subsystem_ptr->vaddr + subsystem_ptr->index)),
					buffer++);
				 subsystem_ptr->index++;

				 length--;
				 bytes_read++;
		   }
		   break;
   }
   /* Most read functions return the number of bytes put into the buffer */
   return bytes_read;
}


/*  Called when a process writes to dev file: echo "hi" > /dev/hello */
static ssize_t device_write(struct file *filp,
   const char *buffer,
   size_t length,
   loff_t *off)
{
   struct subsystem* subsystem_ptr = filp->private_data;

   switch (subsystem_ptr->id)
   {
       case FPGA_DATA_ID:
           if(fpga_download_buffer == NULL)
               fpga_download_buffer = kmalloc(FPGA_DOWNLOAD_BUFFER_SIZE, GFP_KERNEL);
               //buffer for download
           if((fpga_buffer_index + length) > FPGA_DOWNLOAD_BUFFER_SIZE) {
               printk("FPGA download buffer overflow");
               fpga_buffer_index = 0;
               break;
           }
           if(copy_from_user(fpga_download_buffer+fpga_buffer_index,buffer,length))
                return -ENOMEM;
           fpga_buffer_index += length;
           break;
       case FPGA_DOWNLOAD_ID:
           if (fpga_buffer_index) {
               FPGA_Config(fpga_download_buffer,fpga_buffer_index);
               kfree(fpga_download_buffer);
               fpga_download_buffer = NULL;
               fpga_buffer_index = 0;
           }
           else
           {
               printk("No data to download\n");
           }
           break;
       case IRQ_0_ID:
           printk("Clear irq\n");
    	   break;
       default:
           printk("Not support yet\n");
           break;
   }
   return length;
}

MODULE_LICENSE("GPL");

module_init(lophilo_init);
module_exit(lophilo_cleanup);
