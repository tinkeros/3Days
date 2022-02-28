#!/bin/tcsh
source filelist.tcsh
set CFlags = "-Os -g3 `sdl2-config --cflags --libs` -lm -fno-omit-frame-pointer"

if ! -e HCRT/HCRT.BIN then
	cp REPL.VBIN HCRT/HCRT.BIN
endif

if ! -e 3d_loader then
  foreach f ( $CFiles )
    gcc $CFlags -c $f -o $f.o  || rm $f.o
  end
else
  foreach f ( $CFiles )
    if $f:t == "jitlib-core.c" then
      if -e $f.obj then
        foreach f2 ( $MyjitFiles )
          set find = `find . -wholename "./$f2" -newer $f.o `
          if($#find) goto compile
        end
      endif
    endif
    set find = `find -wholename $f -newer 3d_loader `
    compile:
    if($#find) gcc $CFlags -c $f -o $f.o || rm $f.o
  end
endif

if ! -e 3d_loader then
  foreach f ( $AsmFiles )
    yasm -f elf64 $f -o $f.o  || rm $f.o
  end
else
  foreach f ( $AsmFiles )
    set find = `find . -wholename "./$f" -newer 3d_loader `
    if($#find) yasm -f elf64 $f -o $f.o || rm $f.o
  end
endif

set Objs = ()
foreach f ( $CFiles )
  set Objs = ( $Objs "$f.o" )
end
foreach f ( $AsmFiles )
  set Objs = ( $Objs "$f.o" )
end
gcc $Objs `sdl2-config --libs` -lm -o 3d_loader
