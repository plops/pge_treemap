for i in README.md source0/{CMakeLists.txt,src/*.hpp,src/*.cpp} .github/workflows/*.yml
do
    echo "// start of "$i
    cat $i
done
