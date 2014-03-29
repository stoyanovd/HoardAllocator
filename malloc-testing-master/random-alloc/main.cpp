#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <pthread.h>

void batch(std::vector<std::vector<char> >& v)
{
    size_t const N = 1000;
    for (size_t j = 0; j != N; ++j)
    {
        size_t const MAX_SIZE = 40000;
        size_t i = rand() % MAX_SIZE;
        if (i < v.size())
        {
            std::swap(v[i], v.back());
            v.pop_back();
        }
        else
        {
            v.push_back(std::vector<char>(rand() % 10000));
        }
    }
}

int main(int argc, char *argv[])
{
    std::vector<std::vector<char> > v;

    for (size_t i = 0; i != 100; ++i)
    {
        auto start = std::chrono::high_resolution_clock::now();
        batch(v);
        auto end = std::chrono::high_resolution_clock::now();
        std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
                  << " milliseconds\n";
    }

    return EXIT_SUCCESS;
}
