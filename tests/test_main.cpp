#include <exception>
#include <iostream>

void run_path_safety_tests();
void run_protocol_tests();
void run_task_tests();

int main() {
    try {
        run_path_safety_tests();
        run_protocol_tests();
        run_task_tests();
        std::cout << "All tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failure: " << exception.what() << '\n';
        return 1;
    }
}
