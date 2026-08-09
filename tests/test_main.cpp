#include <exception>
#include <iostream>

void run_path_safety_tests();
void run_error_mapping_tests();
void run_config_tests();
void run_magnet_metadata_tests();
void run_protocol_tests();
void run_resume_data_tests();
void run_task_tests();
void run_file_priority_tests();
void run_tabbed_details_tests();
void run_user_agent_tests();

int main() {
    try {
        run_path_safety_tests();
        run_error_mapping_tests();
        run_config_tests();
        run_magnet_metadata_tests();
        run_protocol_tests();
        run_resume_data_tests();
        run_task_tests();
        run_file_priority_tests();
        run_tabbed_details_tests();
        run_user_agent_tests();
        std::cout << "All tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failure: " << exception.what() << '\n';
        return 1;
    }
}
