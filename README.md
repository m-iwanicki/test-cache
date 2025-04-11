# Cache attack test

## Preparation

If testing on i7-14700k then copy `i7-14700k` to correct dir:

```
cp files/i7-14700k.h armageddon/libflush/libflush/eviction/strategies/
```

If not then change `LIBFLUSH_CONFIGURATION` when calling make to custom config
or to `default` (more in armageddon repo readme)

## Build

```
make
```

## Run

1. `test_original` - original test without `libflush`
2. `test_libflush` - same but with usage of `libflush`
