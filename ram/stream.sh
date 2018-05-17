gcc -O stream.c -o stream
sudo taskset 2 chrt --fifo 99 ./stream
