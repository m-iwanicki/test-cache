# Cache attack test

## Preparation

Copy configuration files:

```sh
cp files/* armageddon/libflush/libflush/eviction/strategies/
```

To create custom config you can check (replace `index3` with the highest index
available):

```sh
cat /sys/devices/system/cpu/cpu0/cache/index3/{coherency_line_size,number_of_sets}
64
24576
```

and modify

```c
#define NUMBER_OF_SETS 24576
#define LINE_LENGTH_LOG2 6
#define LINE_LENGTH 64
```

in config header accordingly

## Build

Replace `<CONFIG>` with name of header file (without `.h` suffix) that's
available in `armageddon/libflush/libflush/eviction/strategies/`

```sh
make LIBFLUSH_CONFIGURATION=<CONFIG>
```

## Run

Run both `access_shared_mem` and `test_libflush` at the same time.

1. `access_shared_mem` will call `libshared_lib.so` function which accesses
array indices passed as an argument to `access_shared_mem`. First argument
is how many times this function will be called. E.g.:

    ```sh
    LD_LIBRARY_PATH=bin bin/access_shared_mem 18446744073709551615 0 8 16 200
    ```

2. `test_libflush` - `Flush+Reload attack` will try to find which array indices
   were accessed by `shared_lib.so` (called by `access_shared_mem`) with
   resolution being cache line size, e.g. 64 bytes or 8 uint64_t array elements
   on `i7-14700k`.

   Press `CTRL+C` to stop counting cache hits and print results. Offsets in the
   results are from the start of the range. Ranges are taken from
   `/proc/self/maps`

    ```sh
    $ LD_LIBRARY_PATH=bin bin/test_libflush
    Prepare program
    Calibration threshold: 213
    range: 0x7f0382252000-0x7f0382253000
    range: 0x7f0382254000-0x7f0382255000
    range: 0x7f0382255000-0x7f0382256000
    Average time of cache hit and cache miss when accessing shared memory
    Mean cache hit time: 67
    Mean cache miss time: 425

    Flush+Reload:
    Addr          Cache hits      Cache line offset       Byte offset     uint64_t offset
    (...)
    0x7f0382254680  428             +26                     +1664           +208
    0x7f03822540c0  444             +3                      +192            +24
    0x7f0382254000  807             +0                      +0              +0
    0x7f0382254040  809             +1                      +64             +8
    0x7f0382254080  809             +2                      +128            +16
    0x7f0382254640  809             +25                     +1600           +200
    ```

    Cache line offsets might not be correct as on `i5-1240p` it looks like
    address to cache line should be offset by `0x20` (32 bytes or 4 `uint64_t`
    elements). In other words accessing array indices between 4 and 11 will
    result in only one cache line being read.
    Accessing indices `<x, x+8>` e.g. 4 and 12 results for some reason in
    multiple cache line hits (possibly even whole array is cached), but
    when accessing indices 4 and 13 it doesn't happen. Possibly some
    architecture optimization.
