# Cache attack test

## Preparation

If testing on i7-14700k then copy `i7-14700k` to correct dir:

```
cp files/i7-14700k.h armageddon/libflush/libflush/eviction/strategies/
```

If not then change `LIBFLUSH_CONFIGURATION` when calling make to custom config
or to `default` (more in armageddon repo readme)

## Build

```sh
make
```

## Run

Run both `access_shared_mem` and `test_libflush` at the same time.

1. `access_shared_mem` will call `libshared_lib.so` function which accesses
array indices passed as a argument to `access_shared_mem`. First argument
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
