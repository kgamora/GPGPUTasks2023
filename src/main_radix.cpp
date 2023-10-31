#include <libgpu/context.h>
#include <libgpu/shared_device_buffer.h>
#include <libutils/fast_random.h>
#include <libutils/misc.h>
#include <libutils/timer.h>

// Этот файл будет сгенерирован автоматически в момент сборки - см. convertIntoHeader в CMakeLists.txt:18
#include "cl/radix_cl.h"

#include <iostream>
#include <stdexcept>
#include <vector>

static const unsigned int workGroupSize = 128;

#define NUM_BITS 4
#define POWER_OF_BITS (1 << NUM_BITS)
#define NUM_ITERS (32 / NUM_BITS)
#define WG_SIZE 128
#define clz(x) __builtin_clz(x)

int32_t ilog2(uint32_t x) {
    return sizeof(uint32_t) * CHAR_BIT - clz(x) - 1;
}

template<typename T>
void raiseFail(const T &a, const T &b, std::string message, std::string filename, int line) {
    if (a != b) {
        std::cerr << message << " But " << a << " != " << b << ", " << filename << ":" << line << std::endl;
        throw std::runtime_error(message);
    }
}

#define EXPECT_THE_SAME(a, b, message) raiseFail(a, b, message, __FILE__, __LINE__)

void prefix_sum(gpu::gpu_mem_32u &src, gpu::gpu_mem_32u &dest, int n_pow, ocl::Kernel &prefix_sum_reduce,
                ocl::Kernel &prefix_sum_write) {
    int n = 1 << n_pow;

    for (int p = 0; p <= n_pow; ++p) {
        const unsigned int workGroupSize = 128;
        prefix_sum_reduce.exec(gpu::WorkSize(n < workGroupSize ? n : workGroupSize, n), src, p);
        prefix_sum_write.exec(gpu::WorkSize(n < workGroupSize ? n : workGroupSize, n), src, dest, p);
    }
}

void radix_sort(std::vector<unsigned int> &as, std::vector<unsigned int> &bs, timer &t, int n_pow) {
    int n = 1 << n_pow;

    int wgscnt = n / WG_SIZE;
    int cntsz = wgscnt * WG_SIZE;

    ocl::Kernel count(radix_kernel, radix_kernel_length, "count");
    count.compile();
    ocl::Kernel reorder(radix_kernel, radix_kernel_length, "reorder");
    reorder.compile();
    ocl::Kernel prefix_sum_reduce(radix_kernel, radix_kernel_length, "prefix_sum_reduce");
    prefix_sum_reduce.compile();
    ocl::Kernel prefix_sum_write(radix_kernel, radix_kernel_length, "prefix_sum_write");
    prefix_sum_write.compile();
    ocl::Kernel matrix_transpose(radix_kernel, radix_kernel_length, "matrix_transpose");
    matrix_transpose.compile();
    ocl::Kernel reset(radix_kernel, radix_kernel_length, "reset");
    reset.compile();

    gpu::gpu_mem_32u as_gpu;
    as_gpu.resizeN(n);
    as_gpu.writeN(as.data(), n);

    gpu::gpu_mem_32u bs_gpu;
    bs_gpu.resizeN(n);

    gpu::gpu_mem_32u buf;
    buf.resizeN(cntsz);

    gpu::gpu_mem_32u counts;
    counts.resizeN(cntsz);

    gpu::gpu_mem_32u psums;
    psums.resizeN(cntsz);

    t.restart();
    for (uint32_t i = 0; i < NUM_ITERS; i++) {
        // first step - count
        reset.exec(gpu::WorkSize(WG_SIZE, cntsz), counts);
        count.exec(gpu::WorkSize(WG_SIZE, n), as_gpu, counts, i);

        // second step - transpose
        matrix_transpose.exec(gpu::WorkSize(WG_SIZE, cntsz), counts, buf, wgscnt, POWER_OF_BITS);

        // third step - prefix sums
        reset.exec(gpu::WorkSize(WG_SIZE, cntsz), psums);
        prefix_sum(buf, psums, ilog2(cntsz), prefix_sum_reduce, prefix_sum_write);

        // final step - reorder
        reorder.exec(gpu::WorkSize(WG_SIZE, n), as_gpu, bs_gpu, psums, wgscnt, i);

        // all over again
        std::swap(as_gpu, bs_gpu);
    }
    t.nextLap();

    as_gpu.writeN(bs.data(), n);
}


int main(int argc, char **argv) {
    gpu::Device device = gpu::chooseGPUDevice(argc, argv);

    gpu::Context context;
    context.init(device.device_id_opencl);
    context.activate();

    int benchmarkingIters = 10;
    unsigned int n_pow = 25;
    unsigned int n = 1 << n_pow;
    std::vector<unsigned int> as(n, 0);
    std::vector<unsigned int> bs(n, 0);
    FastRandom r(n);
    for (unsigned int i = 0; i < n; ++i) {
        as[i] = (unsigned int) r.next(0, std::numeric_limits<int>::max());
    }
    std::cout << "Data generated for n=" << n << "!" << std::endl;

    std::vector<unsigned int> cpu_sorted;
    {
        timer t;
        for (int iter = 0; iter < benchmarkingIters; ++iter) {
            cpu_sorted = as;
            std::sort(cpu_sorted.begin(), cpu_sorted.end());
            t.nextLap();
        }
        std::cout << "CPU: " << t.lapAvg() << "+-" << t.lapStd() << " s" << std::endl;
        std::cout << "CPU: " << (n / 1000 / 1000) / t.lapAvg() << " millions/s" << std::endl;
    }

    timer t;
    for (int iter = 0; iter < benchmarkingIters; ++iter) {
        radix_sort(as, bs, t, n_pow);
    }
    std::cout << "GPU: " << t.lapAvg() << "+-" << t.lapStd() << " s" << std::endl;
    std::cout << "GPU: " << (n / 1000 / 1000) / t.lapAvg() << " millions/s" << std::endl;

    // Проверяем корректность результатов
    for (int i = 0; i < n; ++i) {
        EXPECT_THE_SAME(bs[i], cpu_sorted[i], "GPU results should be equal to CPU results!");
    }

    return 0;
}
