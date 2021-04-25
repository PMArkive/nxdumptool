#!/bin/bash
cd "$(dirname "${BASH_SOURCE[0]}")"

tar_filename="nxdumptool-rewrite_poc_$(git rev-parse --short HEAD).tar.bz2"

rm -f ./*.tar.bz2

rm -rf ./code_templates/tmp
mkdir ./code_templates/tmp

mv ./source/main.cpp ./main.cpp

make clean_all

for f in ./code_templates/*.c; do
    basename="$(basename "$f")"
    filename="${basename%.*}"
    
    if [[ $filename == "dump_title_infos" ]]; then
        continue
    fi
    
    echo $filename
    
    rm -f ./source/main.c
    cp $f ./source/main.c
    
    make clean
    make -j$(nproc)
    
    mkdir ./code_templates/tmp/$filename
    cp ./nxdumptool.nro ./code_templates/tmp/$filename/nxdumptool.nro
    #cp ./nxdumptool.elf ./code_templates/tmp/$filename/nxdumptool.elf
done

make clean_all

cd ./code_templates/tmp
tar -cjf ../../$tar_filename *

cd ../..
rm -f ./source/main.c
rm -rf ./code_templates/tmp
mv ./main.cpp ./source/main.cpp

read -p "Press any key to finish ..."
