#ifndef SPEEDLIST_H
# define SPEEDLIST_H 1

# if 1 \
 && (!defined(B0) || B0 == 0) \
 && (!defined(B50) || B50 == 50) \
 && (!defined(B75) || B75 == 75) \
 && (!defined(B110) || B110 == 110) \
 && (!defined(B134) || B134 == 134) \
 && (!defined(B150) || B150 == 150) \
 && (!defined(B200) || B200 == 200) \
 && (!defined(B300) || B300 == 300) \
 && (!defined(B600) || B600 == 600) \
 && (!defined(B1200) || B1200 == 1200) \
 && (!defined(B1800) || B1800 == 1800) \
 && (!defined(B2400) || B2400 == 2400) \
 && (!defined(B4800) || B4800 == 4800) \
 && (!defined(B7200) || B7200 == 7200) \
 && (!defined(B9600) || B9600 == 9600) \
 && (!defined(B14400) || B14400 == 14400) \
 && (!defined(B19200) || B19200 == 19200) \
 && (!defined(B28800) || B28800 == 28800) \
 && (!defined(B33600) || B33600 == 33600) \
 && (!defined(B38400) || B38400 == 38400) \
 && (!defined(B57600) || B57600 == 57600) \
 && (!defined(B76800) || B76800 == 76800) \
 && (!defined(B115200) || B115200 == 115200) \
 && (!defined(B153600) || B153600 == 153600) \
 && (!defined(B230400) || B230400 == 230400) \
 && (!defined(B307200) || B307200 == 307200) \
 && (!defined(B460800) || B460800 == 460800) \
 && (!defined(B500000) || B500000 == 500000) \
 && (!defined(B576000) || B576000 == 576000) \
 && (!defined(B614400) || B614400 == 614400) \
 && (!defined(B921600) || B921600 == 921600) \
 && (!defined(B1000000) || B1000000 == 1000000) \
 && (!defined(B1152000) || B1152000 == 1152000) \
 && (!defined(B1500000) || B1500000 == 1500000) \
 && (!defined(B2000000) || B2000000 == 2000000) \
 && (!defined(B2500000) || B2500000 == 2500000) \
 && (!defined(B3000000) || B3000000 == 3000000) \
 && (!defined(B3500000) || B3500000 == 3500000) \
 && (!defined(B4000000) || B4000000 == 4000000) \
 && (!defined(B5000000) || B5000000 == 5000000) \
 && (!defined(B10000000) || B10000000 == 10000000) \

#  define TERMIOS_SPEED_T_SANE 1

# endif

ATTRIBUTE_CONST
static unsigned long int
baud_to_value (speed_t speed)
{
# ifdef TERMIOS_SPEED_T_SANE
  return speed;
# else
  switch (speed)
    {
#  ifdef B0
      case B0: return 0;
#  endif
#  ifdef B50
      case B50: return 50;
#  endif
#  ifdef B75
      case B75: return 75;
#  endif
#  ifdef B110
      case B110: return 110;
#  endif
#  ifdef B134
      case B134: return 134;
#  endif
#  ifdef B150
      case B150: return 150;
#  endif
#  ifdef B200
      case B200: return 200;
#  endif
#  ifdef B300
      case B300: return 300;
#  endif
#  ifdef B600
      case B600: return 600;
#  endif
#  ifdef B1200
      case B1200: return 1200;
#  endif
#  ifdef B1800
      case B1800: return 1800;
#  endif
#  ifdef B2400
      case B2400: return 2400;
#  endif
#  ifdef B4800
      case B4800: return 4800;
#  endif
#  ifdef B7200
      case B7200: return 7200;
#  endif
#  ifdef B9600
      case B9600: return 9600;
#  endif
#  ifdef B14400
      case B14400: return 14400;
#  endif
#  ifdef B19200
      case B19200: return 19200;
#  endif
#  ifdef B28800
      case B28800: return 28800;
#  endif
#  ifdef B33600
      case B33600: return 33600;
#  endif
#  ifdef B38400
      case B38400: return 38400;
#  endif
#  ifdef B57600
      case B57600: return 57600;
#  endif
#  ifdef B76800
      case B76800: return 76800;
#  endif
#  ifdef B115200
      case B115200: return 115200;
#  endif
#  ifdef B153600
      case B153600: return 153600;
#  endif
#  ifdef B230400
      case B230400: return 230400;
#  endif
#  ifdef B307200
      case B307200: return 307200;
#  endif
#  ifdef B460800
      case B460800: return 460800;
#  endif
#  ifdef B500000
      case B500000: return 500000;
#  endif
#  ifdef B576000
      case B576000: return 576000;
#  endif
#  ifdef B614400
      case B614400: return 614400;
#  endif
#  ifdef B921600
      case B921600: return 921600;
#  endif
#  ifdef B1000000
      case B1000000: return 1000000;
#  endif
#  ifdef B1152000
      case B1152000: return 1152000;
#  endif
#  ifdef B1500000
      case B1500000: return 1500000;
#  endif
#  ifdef B2000000
      case B2000000: return 2000000;
#  endif
#  ifdef B2500000
      case B2500000: return 2500000;
#  endif
#  ifdef B3000000
      case B3000000: return 3000000;
#  endif
#  ifdef B3500000
      case B3500000: return 3500000;
#  endif
#  ifdef B4000000
      case B4000000: return 4000000;
#  endif
#  ifdef B5000000
      case B5000000: return 5000000;
#  endif
#  ifdef B10000000
      case B10000000: return 10000000;
#  endif
      default: return -1;
    }
# endif
}

ATTRIBUTE_CONST
static speed_t
value_to_baud (unsigned long int value)
{
# ifdef TERMIOS_SPEED_T_SANE
  speed_t speed = value;
  if (speed != value)
    speed = (speed_t) -1;	/* Unrepresentable (overflow?) */
  return speed;
# else
  switch (value)
    {
#  ifdef B0
      case 0: return B0;
#  endif
#  ifdef B50
      case 50: return B50;
#  endif
#  ifdef B75
      case 75: return B75;
#  endif
#  ifdef B110
      case 110: return B110;
#  endif
#  ifdef B134
      case 134: return B134;
#  endif
#  ifdef B150
      case 150: return B150;
#  endif
#  ifdef B200
      case 200: return B200;
#  endif
#  ifdef B300
      case 300: return B300;
#  endif
#  ifdef B600
      case 600: return B600;
#  endif
#  ifdef B1200
      case 1200: return B1200;
#  endif
#  ifdef B1800
      case 1800: return B1800;
#  endif
#  ifdef B2400
      case 2400: return B2400;
#  endif
#  ifdef B4800
      case 4800: return B4800;
#  endif
#  ifdef B7200
      case 7200: return B7200;
#  endif
#  ifdef B9600
      case 9600: return B9600;
#  endif
#  ifdef B14400
      case 14400: return B14400;
#  endif
#  ifdef B19200
      case 19200: return B19200;
#  endif
#  ifdef B28800
      case 28800: return B28800;
#  endif
#  ifdef B33600
      case 33600: return B33600;
#  endif
#  ifdef B38400
      case 38400: return B38400;
#  endif
#  ifdef B57600
      case 57600: return B57600;
#  endif
#  ifdef B76800
      case 76800: return B76800;
#  endif
#  ifdef B115200
      case 115200: return B115200;
#  endif
#  ifdef B153600
      case 153600: return B153600;
#  endif
#  ifdef B230400
      case 230400: return B230400;
#  endif
#  ifdef B307200
      case 307200: return B307200;
#  endif
#  ifdef B460800
      case 460800: return B460800;
#  endif
#  ifdef B500000
      case 500000: return B500000;
#  endif
#  ifdef B576000
      case 576000: return B576000;
#  endif
#  ifdef B614400
      case 614400: return B614400;
#  endif
#  ifdef B921600
      case 921600: return B921600;
#  endif
#  ifdef B1000000
      case 1000000: return B1000000;
#  endif
#  ifdef B1152000
      case 1152000: return B1152000;
#  endif
#  ifdef B1500000
      case 1500000: return B1500000;
#  endif
#  ifdef B2000000
      case 2000000: return B2000000;
#  endif
#  ifdef B2500000
      case 2500000: return B2500000;
#  endif
#  ifdef B3000000
      case 3000000: return B3000000;
#  endif
#  ifdef B3500000
      case 3500000: return B3500000;
#  endif
#  ifdef B4000000
      case 4000000: return B4000000;
#  endif
#  ifdef B5000000
      case 5000000: return B5000000;
#  endif
#  ifdef B10000000
      case 10000000: return B10000000;
#  endif
      default: return (speed_t) -1;
    }
# endif
}

#endif
