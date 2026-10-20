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

struct midi_device 
{
	struct usb_device *udev;
	struct usb_interface *interface;
    unsigned char *bulk_in_buffer;
	__u16 bulk_in_packet_size;
	__u8 bulk_in_endpointAddr;
	struct kref refCount;
	struct task_struct *urb_test;
	struct urb *urb;
};

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
	kfree(dev->bulk_in_buffer);
	kfree(dev);
}

static const struct file_operations midi_fops = {
	.owner = THIS_MODULE,
};

static struct usb_class_driver midi_class = {
	.name =	"midi%d",
	.fops =	&midi_fops,
	.minor_base = MIDI_MINOR_BASE,
};

static void midi_bulk_in_callback(struct urb *urb)
{
	struct midi_device *dev = urb->context;
	char hex_string[129];
	printk(KERN_DEBUG "Received %d bytes\n", urb->actual_length);
	printk(KERN_DEBUG "Received bytes\n");
	memset(hex_string, 0, sizeof(hex_string));
	for (size_t i = 0; i < urb->actual_length*2; i+=2)
	{
		unsigned char d = dev->bulk_in_buffer[i/2] >> 4;
		unsigned char r = dev->bulk_in_buffer[i/2] & 0x0f;
		d += '0';
		r += '0';
		if (d > '9') d += 7;
		if (r > '9') r += 7;
		hex_string[i] = d;
		hex_string[i+1] = r;
	}
	hex_string[urb->actual_length*2] = '\0';
	printk(KERN_DEBUG "%s\n", hex_string);
	usb_submit_urb(urb, GFP_ATOMIC);
}

static int run_test_bulk_in(void *data)
{
	struct midi_device *dev = (struct midi_device *)data;
	int retval;
	usb_fill_bulk_urb(dev->urb, dev->udev, usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr), dev->bulk_in_buffer, dev->bulk_in_packet_size, midi_bulk_in_callback, dev);
	retval = usb_submit_urb(dev->urb, GFP_KERNEL);
	while (!kthread_should_stop())
	{
		ssleep(1);
	}

	return 0;
}

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
	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = usb_get_intf(interface);
	dev->urb = usb_alloc_urb(0, GFP_KERNEL);
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
			dev->bulk_in_buffer = kmalloc(dev->bulk_in_packet_size, GFP_KERNEL);
			if (!dev->bulk_in_buffer)
			{
				retval = -ENOMEM;
				goto error;
			}
		}
	}
	if (!endpoints_found)
	{
		retval = -ENODEV;
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
	dev->urb_test = kthread_run(run_test_bulk_in, dev, "test_bulk");
	if (IS_ERR(dev->urb_test))
	{
		printk("KERN_ERR Couldn't run urb test task\n");
		usb_set_intfdata(interface, NULL);
		retval = -ENODEV;
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
	if (dev->urb_test)
	{
		kthread_stop(dev->urb_test);
		printk(KERN_DEBUG "Stopped urb test task\n");
	}
	usb_kill_urb(dev->urb);
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