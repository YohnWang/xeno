#include<stdio.h>
#include<chrono>
#include<thread>
#include<print>

int main()
{
    printf("hello world\n");
    std::println("{}", std::chrono::system_clock::now());
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(12345ms);
    std::println("{}", std::chrono::system_clock::now());
}