import numpy as np
import serial

# transmission header size is 17 bytes
# 8 bytes - address
# 8 bytes - data size
# 1 byte - read or write (1 - write, 0 - read)
header_size = 17

ram_size = 1 * 1024 * 1024  # 1MB
ram = np.zeros(ram_size, dtype=np.uint8)
print('ram size: ', ram_size)
ser = serial.Serial('COM5')


def bytes_to_int(bytes):
    return int.from_bytes(bytes, byteorder='little')


ser.write(101)

i = 0
while 1:
    # see if there's any data waiting
    if ser.in_waiting >= header_size:
        # read the header
        addr = bytes_to_int(ser.read(8))
        data_size = bytes_to_int(ser.read(8))
        rw = bytes_to_int(ser.read(1))
        print('addr: ', addr, '\tdata_size: ', data_size, '\trw: ', rw, sep='')

        if (rw > 0):
            while (ser.in_waiting < data_size):
                pass
            i += 1
            # write
            data = ser.read(data_size)
            ram[addr:addr+data_size] = np.frombuffer(data, dtype=np.uint8)
            #if i % 10 == 0:
             #   print(i)
            #print('write: ', data)
        else:
            # read
            data = ram[addr:addr+data_size]
            ser.write(data)
            # print('read: ', data)
