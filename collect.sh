for i in source0/{CMakeLists.txt,src/main.cpp} .github/workflows/*.yml
do
    echo "// start of "$i
    cat $i
done
