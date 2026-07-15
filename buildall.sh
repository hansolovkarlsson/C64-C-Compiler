for f in hello features forward pointers recursion; do
    ./bin/cc64 tests/$f.c -o tests/$f.asm
    ./bin/c64asm tests/$f.asm -o tests/$f.prg --listing tests/$f.lst
#    python3 bin/mini6502.py tests/$f.prg tests/$f.lst
done
