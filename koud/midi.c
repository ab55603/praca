#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/kthread.h>

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
	struct urb *urb;
	wait_queue_head_t queue;
	unsigned char *ring_buffer;
	unsigned char *read_ptr;
	unsigned char *write_ptr;
	__u16 bulk_out_max_size;
	__u8 bulk_out_endpointAddr;
	struct urb *out_urb;
	unsigned char *bulk_out_buffer;
	wait_queue_head_t write_queue;
	int ongoing_write;
};
static struct usb_driver midi_driver;

static const struct usb_device_id midi_table[] = {
	{ USB_INTERFACE_INFO(MIDI_INTERFACE_CLASS, MIDI_INTERFACE_SUBCLASS, MIDI_INTERFACE_PROTOCOL) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, midi_table);

static void midi_delete(struct kref *refCount)
{
	struct midi_device *dev = container_of(refCount, struct midi_device, refCount);
	usb_put_intf(dev->interface);
	usb_put_dev(dev->udev);
	usb_free_urb(dev->urb);
	usb_free_coherent(dev->udev, dev->out_urb->transfer_buffer_length, dev->out_urb->transfer_buffer, dev->out_urb->transfer_dma);
	usb_free_urb(dev->out_urb);
	kfree(dev->bulk_in_buffer);
	kfree(dev->ring_buffer);
	kfree(dev);
}

static void midi_bulk_in_callback(struct urb *urb)
{
	struct midi_device *dev = urb->context;
	if (urb->status)
	{
		printk(KERN_DEBUG "urb finished with status %d\n", urb->status);
		return;
	}
	printk(KERN_DEBUG "Received %d bytes\n", urb->actual_length);
	for (size_t i = 0; i < urb->actual_length; i+=MIDI_EVENT_SIZE)
	{
		if (dev->bulk_in_buffer[i] == 0) break;
		memcpy(dev->write_ptr, dev->bulk_in_buffer+i, MIDI_EVENT_SIZE);
		dev->write_ptr += MIDI_EVENT_SIZE;
		dev->write_ptr = dev->ring_buffer+(dev->write_ptr-dev->ring_buffer) % (MIDI_EVENT_SIZE*NO_OF_EVENTS);
	}
	wake_up_interruptible(&dev->queue);
	usb_submit_urb(urb, GFP_ATOMIC);
}

static int midi_open(struct inode *inode, struct file *file)
{
	printk(KERN_DEBUG "MIDI open function called\n");
	struct midi_device *dev;
	struct usb_interface *interface;
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
	dev->write_ptr = dev->ring_buffer;
	dev->read_ptr = dev->ring_buffer;
	usb_fill_bulk_urb(dev->urb, dev->udev, usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr), dev->bulk_in_buffer, dev->bulk_in_packet_size, midi_bulk_in_callback, dev);
	retval = usb_submit_urb(dev->urb, GFP_KERNEL);
	if (retval)
	{
		printk(KERN_ERR "MIDI open: failed submitting rcvbulk, error %d\n", retval);
		goto exit;
	}
	file->private_data = dev;
	kref_get(&dev->refCount);

exit:
	return retval;
}

static int midi_release(struct inode *inode, struct file *file)
{
	printk(KERN_DEBUG "MIDI release function called\n");
	struct midi_device *dev;
	dev = file->private_data;
	usb_kill_urb(dev->urb);
	usb_kill_urb(dev->out_urb);
	dev->read_ptr = dev->ring_buffer;
	dev->write_ptr = dev->ring_buffer;
	dev->ongoing_write = 0;
	kref_put(&dev->refCount, midi_delete);

	return 0;
}

static ssize_t midi_read(struct file *file, char __user *buf, size_t count, loff_t *off)
{
	// printk(KERN_DEBUG "MIDI read function called\n");
	struct midi_device *dev;
	dev = file->private_data;
	ssize_t retval = 0;
	if (count < MIDI_EVENT_SIZE)
	{
		retval = -EINVAL;
		goto exit;
	}
	size_t remainder = count % MIDI_EVENT_SIZE;
	if (remainder != 0)
		count -= remainder;
	retval = wait_event_interruptible(dev->queue, dev->read_ptr != dev->write_ptr);
	if (retval < 0)
		goto exit;
	size_t offset = dev->write_ptr > dev->read_ptr ? dev->write_ptr-dev->read_ptr : MIDI_EVENT_SIZE*NO_OF_EVENTS-(dev->read_ptr-dev->ring_buffer);
	if (count > offset)
		count = offset;
	if (copy_to_user(buf, dev->read_ptr, count))
	{
		retval = -EFAULT;
		printk(KERN_ERR "can't send data to user\n");
		goto exit;
	}
	dev->read_ptr += count;
	dev->read_ptr = dev->ring_buffer+(dev->read_ptr-dev->ring_buffer) % (MIDI_EVENT_SIZE*NO_OF_EVENTS);
	retval = count;

exit:
	return retval;
}

static void midi_bulk_out_callback(struct urb *urb)
{
	struct midi_device *dev = urb->context;
	if (urb->status)
	{
		dev->ongoing_write = 0;
		printk(KERN_DEBUG "urb finished with status %d\n", urb->status);
		return;
	}
	dev->ongoing_write = 0;
	wake_up_interruptible(&dev->write_queue);
}

static ssize_t midi_write(struct file *file, const char __user *buf, size_t count, loff_t *off)
{
	// printk(KERN_DEBUG "MIDI write function called\n");
	struct midi_device *dev;
	dev = file->private_data;
	ssize_t retval = 0;
	if (count == 0)
		goto exit;
	if (count < 4)
	{
		retval = -EINVAL;
		goto exit;
	}
	if (count > 4)
		count = 4;
	retval = wait_event_interruptible(dev->write_queue, !dev->ongoing_write);
	if (retval < 0)
		goto exit;
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
	printk(KERN_DEBUG "MIDI probe function called\n");
	printk(KERN_DEBUG "minor base defined: %d\n", MIDI_MINOR_BASE);
	struct midi_device *dev;
	int retval;
	int endpoints_found = 0;
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	kref_init(&dev->refCount);
	init_waitqueue_head(&dev->queue);
	init_waitqueue_head(&dev->write_queue);
	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = usb_get_intf(interface);
	dev->urb = usb_alloc_urb(0, GFP_KERNEL);
	dev->out_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->urb)
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
			dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_in_packet_size = le16_to_cpu(endpoint->wMaxPacketSize);
		}
		if (((endpoint->bmAttributes & 0x03) == 0x02) && !(endpoint->bEndpointAddress & 0x80))
		{
			endpoints_found++;
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_out_max_size = le16_to_cpu(endpoint->wMaxPacketSize);
		}
	}
	if (!endpoints_found)
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
	printk(KERN_DEBUG "allocated %dB for bulk in transactions at endpoint addr 0x%02x (IN)\n", dev->bulk_in_packet_size, dev->bulk_in_endpointAddr);
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
	printk(KERN_DEBUG "MIDI disconnect function called\n");
	struct midi_device *dev;
	dev = usb_get_intfdata(interface);
	usb_kill_urb(dev->urb);
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

module_usb_driver(midi_driver); /* instead of init and exit */

MODULE_LICENSE("GPL");