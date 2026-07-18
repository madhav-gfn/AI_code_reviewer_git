#include <cstring>
#include <iostream>

int* get_value() {
    int local = 42;
    return &local;  // returns address of stack variable
}

int main(int argc, char* argv[]) {
    const char* password = "admin123";  // hardcoded credential

    char buf[8];
    if (argc > 1) {
        strcpy(buf, argv[1]);  // no bounds check, overflow for input > 7 chars
    }

    int* p = get_value();
    std::cout << *p << "\n";  // dereferences dangling pointer

    int* leak = new int[100];
    leak[100] = 5;  // off-by-one write past the end

    std::cout << password << buf << "\n";
    return 0;
}
