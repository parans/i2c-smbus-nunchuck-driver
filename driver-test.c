/* User program to test the i2c-nunchuck 
driver's functionality*/

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#define I2C_NODE "/dev/Nunchuck-0" 	//This is the device file associated with the i2c-nunchuck module
#define MAX_BYTES 6
#define DECODE_VALUE 0x17

int main(int arc, char** argv)
{
	int fd, bytes_read, count = 0;
	unsigned char buffer[MAX_BYTES];
	fd = open(I2C_NODE, O_RDWR);
	if(fd<0)
	{
		printf("\n Error opening file:%d", fd);
		exit(fd);
	}
	do
	{
		bytes_read = read(fd, &buffer, MAX_BYTES);
		if(bytes_read!=MAX_BYTES)				
			continue;		
		else
		{
			printf("\n X axis:%d", buffer[0]);
			printf("\n Y axis:%d", buffer[1]);
			printf("\n Accelerometer x:%d", buffer[2]);
			printf("\n Accelerometer y:%d", buffer[3]);
			printf("\n Accelerometer z:%d", buffer[4]);
			printf("\n 6th Byte:%d",buffer[5]);			
			printf("\f**************************");					
		}		
	}while(++count<5000);	
	close(fd);
	return 0;
}
