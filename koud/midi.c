#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/types.h>

#define MIDI_INTERFACE_CLASS 1
#define MIDI_INTERFACE_SUBCLASS 3
#define MIDI_INTERFACE_PROTOCOL 0

#ifdef CONFIG_USB_DYNAMIC_MINORS
	#define MIDI_MINOR_BASE 0
#else
	#define MIDI_MINOR_BASE 192
#endif

#define MIDI_EVENT_SIZE 4
#define NO_OF_EVENTS 64

struct midi_device 
{
	struct usb_device *udev;
	struct usb_interface *interface;
    unsigned char *bulk_in_buffer;
	__u16 bulk_in_packet_size;
	__u8 bulk_in_endpointAddr;
	struct kref refCount;
	struct urb *in_urb;
	wait_queue_head_t read_queue;
	unsigned char *ring_buffer;
	unsigned char *write_ptr;
	__u16 bulk_out_max_size;
	__u8 bulk_out_endpointAddr;
	struct urb *out_urb;
	unsigned char *bulk_out_buffer;
	wait_queue_head_t write_queue;
	struct mutex io_mutex;
	struct mutex write_mutex;
	unsigned int readers;
	unsigned int writers;
	int ongoing_write;
	bool disconnected;
};

struct midi_file_data
{
	struct midi_device *dev;
	unsigned char *read_ptr;
	bool reader;
	bool writer;
};

static struct usb_driver midi_driver;

static const struct usb_device_id midi_table[] = {
	{ USB_INTERFACE_INFO(MIDI_INTERFACE_CLASS, MIDI_INTERFACE_SUBCLASS, MIDI_INTERFACE_PROTOCOL) },
	{ }
};
MODULE_DEVICE_TABLE(usb, midi_table);

static void midi_delete(struct kref *refCount)
{
	struct midi_device *dev = container_of(refCount, struct midi_device, refCount);
	usb_put_intf(dev->interface);
	usb_free_urb(dev->in_urb);
	usb_free_coherent(dev->udev, MIDI_EVENT_SIZE, dev->out_urb->transfer_buffer, dev->out_urb->transfer_dma);
	usb_free_urb(dev->out_urb);
	usb_put_dev(dev->udev);
	kfree(dev->bulk_in_buffer);
	kfree(dev->ring_buffer);
	kfree(dev);
}

static void midi_bulk_in_callback(struct urb *urb)
{
	struct midi_device *dev = urb->context;
	if (urb->status && !(urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN))
	{
		printk(KERN_ERR"read urb finished with status %d\n", urb->status);
	}
	for (size_t i = 0; i < urb->actual_length; i += MIDI_EVENT_SIZE)
	{
		if (dev->bulk_in_buffer[i] == 0) break;
		memcpy(dev->write_ptr, dev->bulk_in_buffer+i, MIDI_EVENT_SIZE);
		dev->write_ptr += MIDI_EVENT_SIZE;
		dev->write_ptr = dev->ring_buffer+(dev->write_ptr-dev->ring_buffer) % (MIDI_EVENT_SIZE*NO_OF_EVENTS);
	}
	wake_up_interruptible(&dev->read_queue);
	usb_submit_urb(urb, GFP_ATOMIC);
}

static int midi_open(struct inode *inode, struct file *file)
{
	struct midi_device *dev;
	struct usb_interface *interface;
	struct midi_file_data *data;
	int minor;
	int retval = 0;
	minor = iminor(inode);
	interface = usb_find_interface(&midi_driver, minor);
	if (!interface)
	{
		printk(KERN_ERR "can't find device for minor %d\n", minor);
		retval = -ENODEV;
		goto exit;
	}
	dev = usb_get_intfdata(interface);
	if (!dev)
	{
		retval = -ENODEV;
		goto exit;
	}
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
	{
		retval = -ENOMEM;
		goto exit;
	}
	data->dev = dev;
	data->read_ptr = dev->write_ptr;
	if (file->f_mode & FMODE_READ)
		data->reader = true;
	if (file->f_mode & FMODE_WRITE)
		data->writer = true;
	mutex_lock(&dev->io_mutex);
	if (data->reader)
		dev->readers++;
	if (data->writer)
		dev->writers++;
	if (dev->readers != 1)
	{
		mutex_unlock(&dev->io_mutex);
		goto skip_urb;
	}
	mutex_unlock(&dev->io_mutex);
	usb_fill_bulk_urb(dev->in_urb, dev->udev, usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr), dev->bulk_in_buffer, dev->bulk_in_packet_size, midi_bulk_in_callback, dev);
	retval = usb_submit_urb(dev->in_urb, GFP_KERNEL);
	if (retval)
	{
		mutex_lock(&dev->io_mutex);
		dev->readers--;
		mutex_unlock(&dev->io_mutex);
		printk(KERN_ERR "MIDI open: failed submitting rcvbulk, error %d\n", retval);
		goto exit;
	}
skip_urb:
	file->private_data = data;
	kref_get(&dev->refCount);

exit:
	return retval;
}

static int midi_release(struct inode *inode, struct file *file)
{
	struct midi_device *dev;
	struct midi_file_data *data;
	data = file->private_data;
	dev = data->dev;
	bool kill = false;
	mutex_lock(&dev->io_mutex);
	if (data->reader)
		dev->readers--;
	if (!dev->readers)
		kill = true;
	if (data->writer)
		dev->writers--;
	mutex_unlock(&dev->io_mutex);
	if (kill)
		usb_kill_urb(dev->in_urb);
	kfree(data);
	kref_put(&dev->refCount, midi_delete);

	return 0;
}

static ssize_t midi_read(struct file *file, char __user *buf, size_t count, loff_t *off)
{
	struct midi_device *dev;
	struct midi_file_data *data;
	data = file->private_data;
	dev = data->dev;
	int retval = 0;
	if (count < MIDI_EVENT_SIZE)
	{
		retval = -EINVAL;
		goto exit;
	}
	size_t remainder = count % MIDI_EVENT_SIZE;
	if (remainder != 0)
		count -= remainder;
	retval = wait_event_interruptible(dev->read_queue, data->read_ptr != dev->write_ptr || dev->disconnected);
	if (retval < 0)
		goto exit;
	mutex_lock(&dev->io_mutex);
	if (dev->disconnected)
	{
		mutex_unlock(&dev->io_mutex);
		retval = -ENODEV;
		goto exit;
	}
	mutex_unlock(&dev->io_mutex);
	size_t offset = dev->write_ptr > data->read_ptr ? dev->write_ptr-data->read_ptr : MIDI_EVENT_SIZE*NO_OF_EVENTS-(data->read_ptr-dev->ring_buffer);
	if (count > offset)
		count = offset;
	if (copy_to_user(buf, data->read_ptr, count))
	{
		retval = -EFAULT;
		printk(KERN_ERR "can't send data to user\n");
		goto exit;
	}
	data->read_ptr += count;
	data->read_ptr = dev->ring_buffer+(data->read_ptr-dev->ring_buffer) % (MIDI_EVENT_SIZE*NO_OF_EVENTS);
	retval = count;

exit:
	return retval;
}

static void midi_bulk_out_callback(struct urb *urb)
{
	struct midi_device *dev = urb->context;
	if (urb->status && !(urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN))
	{
		printk(KERN_DEBUG "write urb finished with status %d\n", urb->status);
		dev->ongoing_write = urb->status;
		wake_up_interruptible(&dev->write_queue);
		return;
	}
	dev->ongoing_write = 0;
	wake_up_interruptible(&dev->write_queue);
}

static ssize_t midi_write(struct file *file, const char __user *buf, size_t count, loff_t *off)
{
	struct midi_device *dev;
	struct midi_file_data *data;
	data = file->private_data;
	dev = data->dev;
	ssize_t retval = 0;
	if (count == 0)
		return retval;
	if (count < 4)
	{
		retval = -EINVAL;
		return retval;
	}
	if (count > 4)
		count = 4;
retry:
	retval = wait_event_interruptible(dev->write_queue, dev->ongoing_write < 1 || dev->disconnected);
	if (retval < 0)
		return retval;
	retval = mutex_lock_interruptible(&dev->write_mutex);
	if (retval < 0)
		goto exit;
	mutex_lock(&dev->io_mutex);
	if (dev->disconnected)
	{
		mutex_unlock(&dev->io_mutex);
		retval = -ENODEV;
		goto exit;
	}
	mutex_unlock(&dev->io_mutex);
	if (dev->ongoing_write != 0)
	{
		if (dev->ongoing_write == 1)
		{
			mutex_unlock(&dev->write_mutex);
			goto retry;
		}
		retval = dev->ongoing_write;
		goto exit;
	}
	dev->ongoing_write = 1;
	if (copy_from_user(dev->bulk_out_buffer, buf, count))
	{
		retval = -EFAULT;
		dev->ongoing_write = 0;
		printk(KERN_ERR "can't get data from user\n");
		goto exit;
	}
	usb_fill_bulk_urb(dev->out_urb, dev->udev, usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr), dev->bulk_out_buffer, count, midi_bulk_out_callback, dev);
	dev->out_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	retval = usb_submit_urb(dev->out_urb, GFP_KERNEL);
	if (retval)
	{
		printk(KERN_ERR "failed submitting sndbulk, error %ld\n", retval);
		dev->ongoing_write = 0;
		goto exit;
	}
	retval = count;

exit:
	mutex_unlock(&dev->write_mutex);

	return retval;
}

static const struct file_operations midi_fops = {
	.owner = THIS_MODULE,
	.read = midi_read,
	.write = midi_write,
	.open = midi_open,
	.release = midi_release,
	.llseek = noop_llseek,
};

static struct usb_class_driver midi_class = {
	.name =	"midi%d",
	.fops =	&midi_fops,
	.minor_base = MIDI_MINOR_BASE,
};

static int midi_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct midi_device *dev;
	int retval;
	int endpoints_found = 0;
	int endpoint_in = 0;
	int endpoint_out = 0;
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	kref_init(&dev->refCount);
	mutex_init(&dev->io_mutex);
	mutex_init(&dev->write_mutex);
	init_waitqueue_head(&dev->read_queue);
	init_waitqueue_head(&dev->write_queue);
	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = usb_get_intf(interface);
	dev->in_urb = usb_alloc_urb(0, GFP_KERNEL);
	dev->out_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->in_urb || !dev->out_urb)
	{
		retval = -ENOMEM;
		goto error;
	}
	for (size_t i = 0; i < interface->cur_altsetting->desc.bNumEndpoints; ++i)
	{
		const struct usb_endpoint_descriptor *endpoint = &interface->cur_altsetting->endpoint[i].desc;
		if (((endpoint->bmAttributes & 0x03) == 0x02) && (endpoint->bEndpointAddress & 0x80))
		{
			endpoints_found++;
			endpoint_in = 1;
			dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_in_packet_size = le16_to_cpu(endpoint->wMaxPacketSize);
		}
		if (((endpoint->bmAttributes & 0x03) == 0x02) && !(endpoint->bEndpointAddress & 0x80))
		{
			endpoints_found++;
			endpoint_out = 1;
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_out_max_size = le16_to_cpu(endpoint->wMaxPacketSize);
		}
	}
	if (!endpoint_out || !endpoint_in)
	{
		retval = -ENODEV;
		goto error;
	}
	dev->bulk_in_buffer = kmalloc(dev->bulk_in_packet_size, GFP_KERNEL);
	if (!dev->bulk_in_buffer)
	{
		retval = -ENOMEM;
		goto error;
	}
	dev->bulk_out_buffer = usb_alloc_coherent(dev->udev, MIDI_EVENT_SIZE, GFP_KERNEL, &dev->out_urb->transfer_dma);
	if (!dev->bulk_out_buffer)
	{
		retval = -ENOMEM;
		goto error;
	}
	dev->ring_buffer = kmalloc(NO_OF_EVENTS*MIDI_EVENT_SIZE, GFP_KERNEL);
	if (!dev->ring_buffer)
	{
		retval = -ENOMEM;
		goto error;
	}
	dev->write_ptr = dev->ring_buffer;
	usb_set_intfdata(interface, dev);
	printk(KERN_DEBUG "Device connected: %s %s\n", dev->udev->manufacturer, dev->udev->product);
	retval = usb_register_dev(interface, &midi_class);
	if (retval)
	{
		printk(KERN_ERR "Couldn't register device\n");
		usb_set_intfdata(interface, NULL);
		goto error;
	}
	
	return 0;

error:
	kref_put(&dev->refCount, midi_delete);

	return retval;
}

static void midi_disconnect(struct usb_interface *interface)
{
	struct midi_device *dev;
	dev = usb_get_intfdata(interface);
	mutex_lock(&dev->io_mutex);
	dev->disconnected = true;
	mutex_unlock(&dev->io_mutex);
	wake_up_interruptible(&dev->read_queue);
	wake_up_interruptible(&dev->write_queue);
	usb_kill_urb(dev->in_urb);
	usb_kill_urb(dev->out_urb);
	usb_deregister_dev(interface, &midi_class);
	printk(KERN_DEBUG "Device disconnected: %s %s\n", dev->udev->manufacturer, dev->udev->product);
	kref_put(&dev->refCount, midi_delete);
}

static struct usb_driver midi_driver = {
	.name =	"midi",
	.id_table =	midi_table,
	.probe = midi_probe,
	.disconnect = midi_disconnect,
};

module_usb_driver(midi_driver);

MODULE_LICENSE("GPL v2");