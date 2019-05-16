/* i2c-nunchuck module, a kernel module to interface with a Nunchuck device connected on an i2c-smbus adapter.
This module accesses the functionalities exposed by the i2c-i801 adapter driver to do the same. 
A large portion of this module is derived from i2c-dev.c*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define SLAVE_ADDRESS 0x52
#define COMMAND 0x40
#define VALUE 0x00
#define MAX_BYTES 6
#define WORK_QUEUE_NAME "smbus_reader"
#define ADAPTER_NAME "SMBus I801 adapter at 0400"

/***************This part of the code is retained from i2c-dev.c*******************/

static struct i2c_driver i2cdev_driver;

/*
 * An i2c_dev represents an i2c_adapter ... an I2C or SMBus master, not a
 * slave (i2c_client) with which messages will be exchanged.  It's coupled
 * with a character special file which is accessed by user mode drivers.
 *
 * The list of i2c_dev structures is parallel to the i2c_adapter lists
 * maintained by the driver model, and is updated using notifications
 * delivered to the i2cdev_driver.
 */
struct i2c_dev {
	struct list_head list;
	struct i2c_adapter *adap;
	struct device *dev;
};

#define I2C_MINORS 256

static LIST_HEAD(i2c_dev_list);
static DEFINE_SPINLOCK(i2c_dev_list_lock);


static struct i2c_dev *i2c_dev_get_by_minor(unsigned index)
{
	struct i2c_dev *i2c_dev;

	spin_lock(&i2c_dev_list_lock);
	list_for_each_entry(i2c_dev, &i2c_dev_list, list) {
		if (i2c_dev->adap->nr == index)
			goto found;
	}
	i2c_dev = NULL;
found:
	spin_unlock(&i2c_dev_list_lock);
	return i2c_dev;
}


static struct i2c_dev *get_free_i2c_dev(struct i2c_adapter *adap)
{
	struct i2c_dev *i2c_dev;

	if (adap->nr >= I2C_MINORS) {
		printk(KERN_ERR "i2c-dev: Out of device minors (%d)\n",
		       adap->nr);
		return ERR_PTR(-ENODEV);
	}

	i2c_dev = kzalloc(sizeof(*i2c_dev), GFP_KERNEL);
	if (!i2c_dev)
		return ERR_PTR(-ENOMEM);
	i2c_dev->adap = adap;

	spin_lock(&i2c_dev_list_lock);
	list_add_tail(&i2c_dev->list, &i2c_dev_list);
	spin_unlock(&i2c_dev_list_lock);
	return i2c_dev;
}

static void return_i2c_dev(struct i2c_dev *i2c_dev)
{
	spin_lock(&i2c_dev_list_lock);
	list_del(&i2c_dev->list);
	spin_unlock(&i2c_dev_list_lock);
	kfree(i2c_dev);
}

static ssize_t show_adapter_name(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct i2c_dev *i2c_dev = i2c_dev_get_by_minor(MINOR(dev->devt));

	if (!i2c_dev)
		return -ENODEV;
	return sprintf(buf, "%s\n", i2c_dev->adap->name);
}

static DEVICE_ATTR(name, S_IRUGO, show_adapter_name, NULL);

/*--------------------------------------------------------------------------------*/

/*This describes the job structure,
a new variable of the type read_job is created while submitting a job*/

struct read_job
{
	struct work_struct ws;	
	struct i2c_nunchuck_client* n_client;	
};

/*This is a client structure specific to this module, 
this is equivalent of i2c_client structure of i2c-dev.c*/

struct i2c_nunchuck_client 
{
	struct i2c_client* client;
	struct workqueue_struct* wq;
	short processing;
	short data_ready;
	unsigned char data_buffer[MAX_BYTES];
	spinlock_t lock;	
};

/*This method is the work request handler,
this handler runs on the kernel worker thread, it reads 6 consecutive bytes
*/

static void read_job_handler(struct work_struct* ws)
{
	int res, count = 0;
	unsigned long flags;
	union i2c_smbus_data temp;
	struct read_job* job;

	job = (struct read_job*)ws;
	
	spin_lock_irqsave(&(job->n_client->lock), flags);
	job->n_client->processing = 1;
	job->n_client->data_ready = 0;
	spin_unlock_irqrestore(&(job->n_client->lock), flags);
	
	do
	{
		res = i2c_smbus_xfer(job->n_client->client->adapter, job->n_client->client->addr,
				job->n_client->client->flags,
	      			I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &temp);
		if(res<0)
			break;
		job->n_client->data_buffer[count++] = temp.byte;		
		
	}while(count<MAX_BYTES);
	
	spin_lock_irqsave(&(job->n_client->lock), flags);
	job->n_client->processing = 0;
	if(res<0)
		job->n_client->data_ready = 0;
	else
		job->n_client->data_ready = 1;	
	spin_unlock_irqrestore(&(job->n_client->lock), flags);
}

/*file_operation open(); this method initializes i2c_client structure,
creates a work queue, sets up the slave address and initializes the device*/

static int i2cnunchuck_open_initialize(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);
	struct i2c_nunchuck_client* n_client;
	struct i2c_adapter* adap;
	struct i2c_dev* i2c_dev;
	union i2c_smbus_data data;	
	int res;
        
	i2c_dev = i2c_dev_get_by_minor(minor);
	if (!i2c_dev)
		return -ENODEV;
		
	adap = i2c_get_adapter(i2c_dev->adap->nr);
	if (!adap)
		return -ENODEV;
	
	n_client = kzalloc(sizeof(*n_client), GFP_KERNEL);
	n_client->client = kzalloc(sizeof(*n_client->client), GFP_KERNEL);
	if (!n_client)
	{
		i2c_put_adapter(adap);
		return -ENOMEM;
	}
	snprintf(n_client->client->name, I2C_NAME_SIZE, "Nunchuck-%d", 0);
	n_client->client->driver = &i2cdev_driver;

	n_client->client->adapter = adap;

	n_client->wq = create_singlethread_workqueue(WORK_QUEUE_NAME);
	if (!n_client->wq)			
		return -ENOMEM;
    
	n_client->processing = 0;
	n_client->data_ready = 0;
	memset(&(n_client->data_buffer), 0, MAX_BYTES);	
	spin_lock_init(&(n_client->lock));		
	
	file->private_data = n_client;
	
	/*Setup slave address*/		
	n_client->client->addr = SLAVE_ADDRESS;

	/*initialize the device*/	
        data.byte = VALUE;        
	res = i2c_smbus_xfer(n_client->client->adapter, n_client->client->addr, n_client->client->flags,
	      I2C_SMBUS_WRITE, COMMAND, I2C_SMBUS_BYTE_DATA, &data);
	if(res<0)
		return -EFAULT;

	return 0;
}

/* file_operation read(); This method performs a non-blocking read(),
it first writes to the device and then submits a read request to the work queue and returns*/
 
static ssize_t i2cnunchuck_wr_rd(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	int res;	
	unsigned long flags;
	struct read_job* new_rj;
	struct i2c_nunchuck_client* n_client = file->private_data;		
	
	/*First write command to the device*/
	res = i2c_smbus_xfer(n_client->client->adapter, n_client->client->addr, 
				n_client->client->flags, I2C_SMBUS_WRITE, VALUE,
				I2C_SMBUS_BYTE, NULL);
	if(res<0)
		return -EFAULT;		
	
	/* submit job to the queue and return*/
				
	new_rj = kmalloc(sizeof(struct read_job), GFP_KERNEL);
	if(new_rj==NULL)	
		return -ENOMEM;	
	
	/*The lock is needed to protect the varaibles n_client->data_ready and n_client->processing
	which are manipulated in the kernel worker thread*/

	spin_lock_irqsave(&(n_client->lock), flags);	
	if(!n_client->data_ready)
	{				
		if(!n_client->processing)
		{			 							
			INIT_WORK((struct work_struct*)new_rj, read_job_handler);
			new_rj->n_client = n_client;
			res = queue_work(n_client->wq, (struct work_struct*)new_rj);			
			if(res == 1)			
				res = -2;
		}
		else	
			res =-1;						
	}
	else
		res = MAX_BYTES;

	spin_unlock_irqrestore(&(n_client->lock), flags);
	/* unlock critical section*/	
	
	if(res==-1)
	{
		//Do not do memory operations within a lock		
		kfree(new_rj);	
	}
	else if(res == MAX_BYTES)
	{
		kfree(new_rj);		
		if (copy_to_user(buf, &(n_client->data_buffer), MAX_BYTES))
			res =-EFAULT;
		n_client->data_ready = 0;		
	}	
	return res;
}

//file_operations close()
static int i2cnunchuck_release(struct inode *inode, struct file *file)
{
	struct i2c_nunchuck_client* n_client = file->private_data;

	destroy_workqueue(n_client->wq);	
	i2c_put_adapter(n_client->client->adapter);
	kfree(n_client->client);
	kfree(n_client);
	file->private_data = NULL;

	return 0;
}

//This module supports only 3 file_operations open(), read(), close()

static const struct file_operations i2c_nunchuck_fops = {
	.owner		= THIS_MODULE,	
	.read		= i2cnunchuck_wr_rd,	
	.open		= i2cnunchuck_open_initialize,
	.release	= i2cnunchuck_release,
};

/* ------------------------------------------------------------------------- */

/* Callback routines, which will be invoked from i2c-core.c, bus.c; 
this routine will attach the client to an adapter, these routines 
are borrowed from i2c-dev.c*/

static struct class *i2c_dev_class;

static int i2cdev_attach_adapter(struct i2c_adapter *adap)
{
	struct i2c_dev *i2c_dev;
	int res;	
	if(strcmp(adap->name, ADAPTER_NAME)!=0)	//create device file only for I2C SMBus adapter, else return			
		return 0;	
	i2c_dev = get_free_i2c_dev(adap);
	if (IS_ERR(i2c_dev))
		return PTR_ERR(i2c_dev);

	/* register this i2c device with the driver core */
	i2c_dev->dev = device_create(i2c_dev_class, &adap->dev,
				     MKDEV(I2C_MAJOR, adap->nr), NULL,
				     "Nunchuck-%d", 0);
	if (IS_ERR(i2c_dev->dev)) {
		res = PTR_ERR(i2c_dev->dev);
		goto error;
	}
	res = device_create_file(i2c_dev->dev, &dev_attr_name);
	if (res)
		goto error_destroy;

	pr_debug("i2c-dev: adapter [%s] registered as minor %d\n",
		 adap->name, adap->nr);
	return 0;
error_destroy:
	device_destroy(i2c_dev_class, MKDEV(I2C_MAJOR, adap->nr));
error:
	return_i2c_dev(i2c_dev);
	return res;
}

static int i2cdev_detach_adapter(struct i2c_adapter *adap)
{
	struct i2c_dev *i2c_dev;

	i2c_dev = i2c_dev_get_by_minor(adap->nr);
	if (!i2c_dev) /* attach_adapter must have failed */
		return 0;

	device_remove_file(i2c_dev->dev, &dev_attr_name);
	return_i2c_dev(i2c_dev);
	device_destroy(i2c_dev_class, MKDEV(I2C_MAJOR, adap->nr));

	pr_debug("i2c-dev: adapter [%s] unregistered\n", adap->name);
	return 0;
}

static struct i2c_driver i2cdev_driver = {
	.driver = {
		.name	= "dev_driver",
	},
	.attach_adapter	= i2cdev_attach_adapter,
	.detach_adapter	= i2cdev_detach_adapter,
};

/* ------------------------------------------------------------------------- */
// This part of the code is same as i2c-dev.c module initialization
// The code is borrowed from i2c-dev.c 
/*
 * module load/unload record keeping
 */

static int __init i2c_dev_init(void)
{
	int res;

	printk(KERN_INFO "i2c /dev entries driver\n");

	res = register_chrdev(I2C_MAJOR, "i2c", &i2c_nunchuck_fops);
	if (res)
		goto out;

	i2c_dev_class = class_create(THIS_MODULE, "i2c-nunchuck"); //Creates i2c-nunchuck module
	
	if (IS_ERR(i2c_dev_class))
	{
		res = PTR_ERR(i2c_dev_class);
		goto out_unreg_chrdev;
	}

	res = i2c_add_driver(&i2cdev_driver);
	if (res)
		goto out_unreg_class;

	return 0;

out_unreg_class:
	class_destroy(i2c_dev_class);
out_unreg_chrdev:
	unregister_chrdev(I2C_MAJOR, "i2c");
out:
	printk(KERN_ERR "%s: Driver Initialisation failed\n", __FILE__);
	return res;
}

static void __exit i2c_dev_exit(void)
{
	i2c_del_driver(&i2cdev_driver);
	class_destroy(i2c_dev_class);
	unregister_chrdev(I2C_MAJOR, "i2c");
}

MODULE_AUTHOR("Frodo Looijaard <frodol@dds.nl> and "
		"Simon G. Vogl <simon@tk.uni-linz.ac.at>" "Modified by Saketh Paranjape");
MODULE_DESCRIPTION("I2C /dev entries driver");
MODULE_LICENSE("GPL");

module_init(i2c_dev_init);
module_exit(i2c_dev_exit);
