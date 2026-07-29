/*
    ChibiOS - Copyright (C) 2006..2015 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "hal.h"
#include "nanovna.h"
#include "usbcfg.h"

/* Virtual serial port over USB.*/
SerialUSBDriver SDU1;

/*
 * Endpoints to be used for USBD1.
 */
#define USBD1_DATA_REQUEST_EP           1
#define USBD1_DATA_AVAILABLE_EP         1
#define USBD1_INTERRUPT_REQUEST_EP      2

/*
 * PMA budget (STM32F303xC has 512 bytes total, see CLAUDE.md).
 *
 *    64  BTABLE (reserved unconditionally by ChibiOS usb_pm_reset)
 *   128  EP0 control          (64 in + 64 out)
 *    64  EP1 in  bulk  CDC    device -> host, kept at 64 for shell throughput
 *    32  EP1 out bulk  CDC    host -> device, commands only, 32 is plenty
 *    16  EP2 in  intr  CDC    notification
 *   196  EP3 in  isoc  audio  49 frames * 2ch * 2 bytes
 *  ----
 *   500  of 512.  Nothing else fits; do not grow any of these.
 */
#define CDC_IN_SIZE                     0x0040
#define CDC_OUT_SIZE                    0x0020
#define CDC_INT_SIZE                    0x0010

/* ChibiOS only asserts this at runtime, and only in debug builds. Catch it here
   instead: an overflow silently corrupts another endpoint's buffer.*/
_Static_assert(64 +                            /* BTABLE                     */
               64 + 64 +                       /* EP0 in + out               */
               CDC_IN_SIZE + CDC_OUT_SIZE +    /* EP1                        */
               CDC_INT_SIZE +                  /* EP2                        */
#ifdef __USB_AUDIO__
               USB_AUDIO_MAX_PACKET +          /* EP3                        */
#endif
               0 <= STM32_USB_PMA_SIZE, "USB PMA budget exceeded");

/* String descriptor indices. 0..3 are lang/vendor/product/serial.*/
#ifdef __USB_AUDIO__
#define USB_STRING_TERMINAL             4
#define USB_STRING_COUNT                5
#else
#define USB_STRING_TERMINAL             0
#define USB_STRING_COUNT                4
#endif

/* Interface numbers.*/
#define IF_CDC_CONTROL                  0
#define IF_CDC_DATA                     1
#ifdef __USB_AUDIO__
#define IF_AUDIO_CONTROL                2
#define IF_AUDIO_STREAM                 3
#define USB_INTERFACES                  4
#else
#define USB_INTERFACES                  2
#endif

/* Audio class descriptor constants.*/
#define UAC_CS_INTERFACE                0x24
#define UAC_CS_ENDPOINT                 0x25
#define UAC_AC_HEADER                   0x01
#define UAC_AC_INPUT_TERMINAL           0x02
#define UAC_AC_OUTPUT_TERMINAL          0x03
#define UAC_AS_GENERAL                  0x01
#define UAC_AS_FORMAT_TYPE              0x02
#define UAC_EP_GENERAL                  0x01

/*
 * Descriptor sizes.  67 bytes is the plain CDC tree; the composite build adds
 * the CDC IAD (8) and the audio function (99, its own IAD included).
 */
#ifdef __USB_AUDIO__
#define USB_CONFIG_SIZE                 (67 + 8 + 99)
#else
#define USB_CONFIG_SIZE                 (67)
#endif

/*
 * USB Device Descriptor.
 *
 * bcdDevice is bumped to 0x0300 so Windows sees a new hardware ID and re-reads
 * the descriptors instead of serving the cached CDC-only set.  idProduct is
 * deliberately unchanged - the python tools in ./python match on 0483:5740.
 */
static const uint8_t vcom_device_descriptor_data[18] = {
#ifdef __USB_AUDIO__
  USB_DESC_DEVICE       (0x0200,        /* bcdUSB (2.0, required for IAD).  */
                         0xEF,          /* bDeviceClass (Miscellaneous).    */
                         0x02,          /* bDeviceSubClass (Common Class).  */
                         0x01,          /* bDeviceProtocol (IAD).           */
#else
  USB_DESC_DEVICE       (0x0110,        /* bcdUSB (1.1).                    */
                         0x02,          /* bDeviceClass (CDC).              */
                         0x00,          /* bDeviceSubClass.                 */
                         0x00,          /* bDeviceProtocol.                 */
#endif
                         0x40,          /* bMaxPacketSize.                  */
                         0x0483,        /* idVendor (ST).                   */
                         0x5740,        /* idProduct.                       */
                         0x0302,        /* bcdDevice.                       */
                         1,             /* iManufacturer.                   */
                         2,             /* iProduct.                        */
                         3,             /* iSerialNumber.                   */
                         1)             /* bNumConfigurations.              */
};

/*
 * Device Descriptor wrapper.
 */
static const USBDescriptor vcom_device_descriptor = {
  sizeof vcom_device_descriptor_data,
  vcom_device_descriptor_data
};

/* Configuration Descriptor tree: CDC (+ USB Audio capture when enabled).*/
static const uint8_t vcom_configuration_descriptor_data[USB_CONFIG_SIZE] = {
  /* Configuration Descriptor.*/
  USB_DESC_CONFIGURATION(USB_CONFIG_SIZE,/* wTotalLength.                   */
                         USB_INTERFACES,/* bNumInterfaces.                  */
                         0x01,          /* bConfigurationValue.             */
                         0,             /* iConfiguration.                  */
                         0xC0,          /* bmAttributes (self powered).     */
                         500 / 2),      /* bMaxPower in 2mA units (500mA).  */
#ifdef __USB_AUDIO__
  /* IAD binding the two CDC interfaces into one function.*/
  USB_DESC_INTERFACE_ASSOCIATION(IF_CDC_CONTROL, 2, 0x02, 0x02, 0x01, 0),
#endif
  /* Interface Descriptor.*/
  USB_DESC_INTERFACE    (IF_CDC_CONTROL,/* bInterfaceNumber.                */
                         0x00,          /* bAlternateSetting.               */
                         0x01,          /* bNumEndpoints.                   */
                         0x02,          /* bInterfaceClass (Communications
                                           Interface Class, CDC section
                                           4.2).                            */
                         0x02,          /* bInterfaceSubClass (Abstract
                                         Control Model, CDC section 4.3).   */
                         0x00,          /* bInterfaceProtocol (No protocol,
                                           CDC section 4.4).                */
                         0),            /* iInterface.                      */
  /* Header Functional Descriptor (CDC section 5.2.3).*/
  USB_DESC_BYTE         (5),            /* bLength.                         */
  USB_DESC_BYTE         (0x24),         /* bDescriptorType (CS_INTERFACE).  */
  USB_DESC_BYTE         (0x00),         /* bDescriptorSubtype (Header
                                           Functional Descriptor.           */
  USB_DESC_BCD          (0x0110),       /* bcdCDC.                          */
  /* Call Management Functional Descriptor. */
  USB_DESC_BYTE         (5),            /* bFunctionLength.                 */
  USB_DESC_BYTE         (0x24),         /* bDescriptorType (CS_INTERFACE).  */
  USB_DESC_BYTE         (0x01),         /* bDescriptorSubtype (Call Management
                                           Functional Descriptor).          */
  USB_DESC_BYTE         (0x00),         /* bmCapabilities (D0+D1).          */
  USB_DESC_BYTE         (IF_CDC_DATA),  /* bDataInterface.                  */
  /* ACM Functional Descriptor.*/
  USB_DESC_BYTE         (4),            /* bFunctionLength.                 */
  USB_DESC_BYTE         (0x24),         /* bDescriptorType (CS_INTERFACE).  */
  USB_DESC_BYTE         (0x02),         /* bDescriptorSubtype (Abstract
                                           Control Management Descriptor).  */
  USB_DESC_BYTE         (0x02),         /* bmCapabilities.                  */
  /* Union Functional Descriptor.*/
  USB_DESC_BYTE         (5),            /* bFunctionLength.                 */
  USB_DESC_BYTE         (0x24),         /* bDescriptorType (CS_INTERFACE).  */
  USB_DESC_BYTE         (0x06),         /* bDescriptorSubtype (Union
                                           Functional Descriptor).          */
  USB_DESC_BYTE         (IF_CDC_CONTROL),/* bMasterInterface (Communication
                                           Class Interface).                */
  USB_DESC_BYTE         (IF_CDC_DATA),  /* bSlaveInterface0 (Data Class
                                           Interface).                      */
  /* Endpoint 2 Descriptor.*/
  USB_DESC_ENDPOINT     (USBD1_INTERRUPT_REQUEST_EP|0x80,
                         0x03,          /* bmAttributes (Interrupt).        */
                         0x0008,        /* wMaxPacketSize.                  */
                         0xFF),         /* bInterval.                       */
  /* Interface Descriptor.*/
  USB_DESC_INTERFACE    (IF_CDC_DATA,   /* bInterfaceNumber.                */
                         0x00,          /* bAlternateSetting.               */
                         0x02,          /* bNumEndpoints.                   */
                         0x0A,          /* bInterfaceClass (Data Class
                                           Interface, CDC section 4.5).     */
                         0x00,          /* bInterfaceSubClass (CDC section
                                            4.6).                           */
                         0x00,          /* bInterfaceProtocol (CDC section
                                            4.7).                           */
                         0x00),         /* iInterface.                      */
  /* Endpoint 3 Descriptor.*/
  USB_DESC_ENDPOINT     (USBD1_DATA_AVAILABLE_EP,       /* bEndpointAddress.*/
                         0x02,          /* bmAttributes (Bulk).             */
                         CDC_OUT_SIZE,  /* wMaxPacketSize.                  */
                         0x00),         /* bInterval.                       */
  /* Endpoint 1 Descriptor.*/
  USB_DESC_ENDPOINT     (USBD1_DATA_REQUEST_EP|0x80,    /* bEndpointAddress.*/
                         0x02,          /* bmAttributes (Bulk).             */
                         CDC_IN_SIZE,   /* wMaxPacketSize.                  */
                         0x00),         /* bInterval.                       */
#ifdef __USB_AUDIO__
  /* IAD binding the two audio interfaces into one function.*/
  USB_DESC_INTERFACE_ASSOCIATION(IF_AUDIO_CONTROL, 2, 0x01, 0x01, 0x00, 0),
  /* Standard AudioControl interface, no endpoints.*/
  USB_DESC_INTERFACE    (IF_AUDIO_CONTROL, 0x00, 0x00, 0x01, 0x01, 0x00, 0),
  /* Class specific AC Interface Header (9 + 12 + 9 = 30 bytes total).*/
  USB_DESC_BYTE         (9),
  USB_DESC_BYTE         (UAC_CS_INTERFACE),
  USB_DESC_BYTE         (UAC_AC_HEADER),
  USB_DESC_BCD          (0x0100),       /* bcdADC.                          */
  USB_DESC_WORD         (30),           /* wTotalLength.                    */
  USB_DESC_BYTE         (1),            /* bInCollection.                   */
  USB_DESC_BYTE         (IF_AUDIO_STREAM),/* baInterfaceNr(1).              */
  /* Input Terminal: the codec ADC pair.
     Line connector, NOT microphone (0x0201).  Windows applies microphone class
     processing - AGC, noise suppression, echo cancellation - to anything that
     declares itself a microphone, which continuously rides the level and makes
     the capture useless for measurement.  A line connector attracts none of
     that.*/
  USB_DESC_BYTE         (12),
  USB_DESC_BYTE         (UAC_CS_INTERFACE),
  USB_DESC_BYTE         (UAC_AC_INPUT_TERMINAL),
  USB_DESC_BYTE         (1),            /* bTerminalID.                     */
  USB_DESC_WORD         (0x0603),       /* wTerminalType (Line connector).  */
  USB_DESC_BYTE         (0),            /* bAssocTerminal.                  */
  USB_DESC_BYTE         (USB_AUDIO_CHANNELS), /* bNrChannels.               */
  USB_DESC_WORD         (0x0003),       /* wChannelConfig (left + right).   */
  USB_DESC_BYTE         (0),            /* iChannelNames.                   */
  USB_DESC_BYTE         (USB_STRING_TERMINAL), /* iTerminal (endpoint name).*/
  /* Output Terminal: USB streaming.*/
  USB_DESC_BYTE         (9),
  USB_DESC_BYTE         (UAC_CS_INTERFACE),
  USB_DESC_BYTE         (UAC_AC_OUTPUT_TERMINAL),
  USB_DESC_BYTE         (2),            /* bTerminalID.                     */
  USB_DESC_WORD         (0x0101),       /* wTerminalType (USB streaming).   */
  USB_DESC_BYTE         (0),            /* bAssocTerminal.                  */
  USB_DESC_BYTE         (1),            /* bSourceID (input terminal).      */
  USB_DESC_BYTE         (0),            /* iTerminal.                       */
  /* Standard AS interface, alt 0: zero bandwidth (stream stopped).*/
  USB_DESC_INTERFACE    (IF_AUDIO_STREAM, 0x00, 0x00, 0x01, 0x02, 0x00, 0),
  /* Standard AS interface, alt 1: streaming.*/
  USB_DESC_INTERFACE    (IF_AUDIO_STREAM, 0x01, 0x01, 0x01, 0x02, 0x00, 0),
  /* Class specific AS interface descriptor.*/
  USB_DESC_BYTE         (7),
  USB_DESC_BYTE         (UAC_CS_INTERFACE),
  USB_DESC_BYTE         (UAC_AS_GENERAL),
  USB_DESC_BYTE         (2),            /* bTerminalLink (output terminal). */
  USB_DESC_BYTE         (1),            /* bDelay (frames).                 */
  USB_DESC_WORD         (0x0001),       /* wFormatTag (PCM).                */
  /* Type I format: 2 channels, 16 bit, one discrete sample rate.*/
  USB_DESC_BYTE         (11),
  USB_DESC_BYTE         (UAC_CS_INTERFACE),
  USB_DESC_BYTE         (UAC_AS_FORMAT_TYPE),
  USB_DESC_BYTE         (0x01),         /* bFormatType (Type I).            */
  USB_DESC_BYTE         (USB_AUDIO_CHANNELS), /* bNrChannels.               */
  USB_DESC_BYTE         (2),            /* bSubframeSize.                   */
  USB_DESC_BYTE         (16),           /* bBitResolution.                  */
  USB_DESC_BYTE         (1),            /* bSamFreqType (one rate).         */
  USB_DESC_BYTE         ((USB_AUDIO_RATE >>  0) & 0xFF),
  USB_DESC_BYTE         ((USB_AUDIO_RATE >>  8) & 0xFF),
  USB_DESC_BYTE         ((USB_AUDIO_RATE >> 16) & 0xFF),
  /* Standard isochronous audio endpoint (9 bytes, adds refresh/synch).*/
  USB_DESC_BYTE         (9),
  USB_DESC_BYTE         (USB_DESCRIPTOR_ENDPOINT),
  USB_DESC_BYTE         (USB_AUDIO_EP|0x80),
  USB_DESC_BYTE         (0x05),         /* Isochronous, asynchronous, data. */
  USB_DESC_WORD         (USB_AUDIO_MAX_PACKET),
  USB_DESC_BYTE         (1),            /* bInterval (every frame).         */
  USB_DESC_BYTE         (0),            /* bRefresh.                        */
  USB_DESC_BYTE         (0),            /* bSynchAddress.                   */
  /* Class specific AS isochronous endpoint descriptor.*/
  USB_DESC_BYTE         (7),
  USB_DESC_BYTE         (UAC_CS_ENDPOINT),
  USB_DESC_BYTE         (UAC_EP_GENERAL),
  USB_DESC_BYTE         (0x00),         /* bmAttributes (no controls).      */
  USB_DESC_BYTE         (0),            /* bLockDelayUnits.                 */
  USB_DESC_WORD         (0),            /* wLockDelay.                      */
#endif
};

/*
 * Configuration Descriptor wrapper.
 */
static const USBDescriptor vcom_configuration_descriptor = {
  sizeof vcom_configuration_descriptor_data,
  vcom_configuration_descriptor_data
};

/*
 * U.S. English language identifier.
 */
static const uint8_t vcom_string0[] = {
  USB_DESC_BYTE(4),                     /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  USB_DESC_WORD(0x0409)                 /* wLANGID (U.S. English).          */
};

/*
 * Vendor string.
 */
static const uint8_t vcom_string1[] = {
  USB_DESC_BYTE(24),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  'n', 0, 'a', 0, 'n', 0, 'o', 0, 'v', 0, 'n', 0, 'a', 0, '.', 0, 'c', 0, 'o', 0, 'm', 0
};

/*
 * Device Description string.
 */
static const uint8_t vcom_string2[] = {
#if defined(NANOVNA_F303)
  USB_DESC_BYTE(16),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  't', 0, 'i', 0, 'n', 0, 'y', 0, 'P', 0, 'F', 0, 'A', 0
#else
  USB_DESC_BYTE(20),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  'N', 0, 'a', 0, 'n', 0, 'o', 0, 'V', 0, 'N', 0, 'A', 0, '-', 0, 'H', 0,
#endif
};

/*
 * Serial Number string.
 */
static const uint8_t vcom_string3[] = {
  USB_DESC_BYTE(8),                     /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  '0' + CH_KERNEL_MAJOR, 0,
  '0' + CH_KERNEL_MINOR, 0,
  '0' + CH_KERNEL_PATCH, 0
};

#ifdef __USB_AUDIO__
/*
 * Input Terminal name.
 *
 * Windows labels a capture endpoint "Microphone" by default; supplying an
 * iTerminal string makes it use this instead.
 */
static const uint8_t vcom_string4[] = {
  USB_DESC_BYTE(32),                    /* bLength (2 + 15 chars * 2).      */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  't', 0, 'i', 0, 'n', 0, 'y', 0, 'P', 0, 'F', 0, 'A', 0, ' ', 0,
  'L', 0, 'i', 0, 'n', 0, 'e', 0, ' ', 0, 'I', 0, 'n', 0
};
#endif

/*
 * Strings wrappers array.
 */
static const USBDescriptor vcom_strings[] = {
  {sizeof vcom_string0, vcom_string0},
  {sizeof vcom_string1, vcom_string1},
  {sizeof vcom_string2, vcom_string2},
  {sizeof vcom_string3, vcom_string3},
#ifdef __USB_AUDIO__
  {sizeof vcom_string4, vcom_string4}
#endif
};

/*
 * Handles the GET_DESCRIPTOR callback. All required descriptors must be
 * handled here.
 */
static const USBDescriptor *get_descriptor(USBDriver *usbp,
                                           uint8_t dtype,
                                           uint8_t dindex,
                                           uint16_t lang) {

  (void)usbp;
  (void)lang;
  switch (dtype) {
  case USB_DESCRIPTOR_DEVICE:
    return &vcom_device_descriptor;
  case USB_DESCRIPTOR_CONFIGURATION:
    return &vcom_configuration_descriptor;
  case USB_DESCRIPTOR_STRING:
    if (dindex < USB_STRING_COUNT)
      return &vcom_strings[dindex];
  }
  return NULL;
}

/**
 * @brief   IN EP1 state.
 */
static USBInEndpointState ep1instate;

/**
 * @brief   OUT EP1 state.
 */
static USBOutEndpointState ep1outstate;

/**
 * @brief   EP1 initialization structure (both IN and OUT).
 */
static const USBEndpointConfig ep1config = {
  USB_EP_MODE_TYPE_BULK,
  NULL,
  sduDataTransmitted,
  sduDataReceived,
  CDC_IN_SIZE,
  CDC_OUT_SIZE,
  &ep1instate,
  &ep1outstate
};

/**
 * @brief   IN EP2 state.
 */
static USBInEndpointState ep2instate;

/**
 * @brief   EP2 initialization structure (IN only).
 */
static const USBEndpointConfig ep2config = {
  USB_EP_MODE_TYPE_INTR,
  NULL,
  sduInterruptTransmitted,
  NULL,
  CDC_INT_SIZE,
  0x0000,
  &ep2instate,
  NULL
};

#ifdef __USB_AUDIO__
/**
 * @brief   Audio IN endpoint state.
 *
 * A staging buffer is unavoidable: rx_buffer is a circular DMA target and a
 * packet may straddle the wrap, while usbStartTransmitI needs one linear run.
 * It is shared with the bulk streaming mode (stream_buffer in main.c) because
 * the two modes are mutually exclusive - see CLAUDE.md, RAM is scarce.
 */
bool usb_audio_active;
static uint8_t usb_audio_alt;
static uint8_t usb_audio_reply;         // GET_INTERFACE reply, must outlive the call
static USBInEndpointState ep3instate;

static const USBEndpointConfig ep3config = {
  USB_EP_MODE_TYPE_ISOC,
  NULL,
  NULL,                                 /* Refilled from the SOF handler.   */
  NULL,
  USB_AUDIO_MAX_PACKET,
  0x0000,
  &ep3instate,
  NULL
};
#endif

/*
 * Handles the USB driver global events.
 */
static void usb_event(USBDriver *usbp, usbevent_t event) {
  extern SerialUSBDriver SDU1;
  /* The PMA allocator only moves forward and is rewound by a bus reset alone,
     so endpoints may be allocated exactly once per reset.  SET_CONFIGURATION
     can legitimately arrive several times in between (notably a 0 to
     de-configure followed by another 1), and with 500 of 512 bytes committed a
     second pass would overflow the packet memory.  usbDisableEndpointsI() is
     not a way out: it rewinds the allocator without re-allocating EP0, so the
     next endpoint would land on top of the control buffers. */
  static bool endpoints_allocated;
  chSysLockFromISR();
  switch (event) {
  case USB_EVENT_RESET:
    endpoints_allocated = false;
#ifdef __USB_AUDIO__
    usb_audio_active = false;
    usb_audio_alt = 0;
#endif
    break;
  case USB_EVENT_ADDRESS:
    break;
  case USB_EVENT_CONFIGURED:
#ifdef __USB_AUDIO__
    usb_audio_active = false;
    usb_audio_alt = 0;
#endif
    if (usbp->configuration == 0)
      break;                            /* De-configured, leave the PMA alone.*/
    /* Enables the endpoints specified into the configuration.
       Note, this callback is invoked from an ISR so I-Class functions
       must be used.*/
    if (!endpoints_allocated) {
      endpoints_allocated = true;
      usbInitEndpointI(usbp, USBD1_DATA_REQUEST_EP, &ep1config);
      usbInitEndpointI(usbp, USBD1_INTERRUPT_REQUEST_EP, &ep2config);
#ifdef __USB_AUDIO__
      usbInitEndpointI(usbp, USB_AUDIO_EP, &ep3config);
#endif
    }
    /* Resetting the state of the CDC subsystem.*/
    sduConfigureHookI(&SDU1);
    break;
  case USB_EVENT_SUSPEND:
    /* Disconnection event on suspend.*/
#ifdef __USB_AUDIO__
    usb_audio_active = false;
#endif
    sduDisconnectI(&SDU1);
    break;
  case USB_EVENT_WAKEUP:
    break;
  case USB_EVENT_STALLED:
    break;
  }
  chSysUnlockFromISR();
  return;
}

/*
 * ChibiOS leaves SET_INTERFACE/GET_INTERFACE unimplemented, but the audio
 * class needs them to switch between alt 0 (zero bandwidth) and alt 1
 * (streaming).  Everything else is passed on to the CDC handler.
 */
static bool vna_requests_hook(USBDriver *usbp) {
#ifdef __USB_AUDIO__
  if (((usbp->setup.bmRequestType & USB_RTYPE_TYPE_MASK) == USB_RTYPE_TYPE_STD) &&
      ((usbp->setup.bmRequestType & USB_RTYPE_RECIPIENT_MASK) == USB_RTYPE_RECIPIENT_INTERFACE)) {
    switch (usbp->setup.bRequest) {
    case USB_REQ_SET_INTERFACE:
      /* wIndex = interface, wValue = alternate setting.*/
      if (usbp->setup.wIndex == IF_AUDIO_STREAM) {
        usb_audio_alt = (uint8_t)usbp->setup.wValue;
        usb_audio_active = (usb_audio_alt != 0);
      }
      usbSetupTransfer(usbp, NULL, 0, NULL);
      return true;
    case USB_REQ_GET_INTERFACE:
      /* Every other interface has only alt 0.*/
      usb_audio_reply = (usbp->setup.wIndex == IF_AUDIO_STREAM) ? usb_audio_alt : 0;
      usbSetupTransfer(usbp, &usb_audio_reply, 1, NULL);
      return true;
    default:
      break;
    }
  }
#endif
  return sduRequestsHook(usbp);
}

/*
 * Handles the USB driver global events.
 */
static void sof_handler(USBDriver *usbp) {
  osalSysLockFromISR();
  sduSOFHookI(&SDU1);
#ifdef __USB_AUDIO__
  /* One packet per frame.  Queue the next one whenever the endpoint went
     idle, which for an isochronous IN endpoint is once per frame.*/
  if (usb_audio_active && (usbp->state == USB_ACTIVE) &&
      !usbGetTransmitStatusI(usbp, USB_AUDIO_EP)) {
    usbStartTransmitI(usbp, USB_AUDIO_EP, stream_buffer,
                      usb_audio_fill(stream_buffer));
  }
#else
  (void)usbp;
#endif
  osalSysUnlockFromISR();
}

/*
 * USB driver configuration.
 */
const USBConfig usbcfg = {
  usb_event,
  get_descriptor,
  vna_requests_hook,
  sof_handler
};

/*
 * Serial over USB driver configuration.
 */
const SerialUSBConfig serusbcfg = {
  &USBD1,
  USBD1_DATA_REQUEST_EP,
  USBD1_DATA_AVAILABLE_EP,
  USBD1_INTERRUPT_REQUEST_EP
};
