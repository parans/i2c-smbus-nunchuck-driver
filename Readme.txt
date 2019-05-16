These are the task 3 files i2c-nunchuck driver

1) i2c-nunchuck.c is the i2c-nunchuck driver
2) i2c-nunchuck.o is the object file
3) i2c-nunchuck.ko is the loadable kernel module
4) driver-test.c is a simple program for testing the i2c-nunchuck driver.
5) The make file is for the i2c-nunchuck.c.
6) There is no make file for the test program driver-test.c

=========================================================================================

Important : On loading i2c-nunchuck module, only 1 device file is created /dev/Nunchuck-0
