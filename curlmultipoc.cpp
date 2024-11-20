// curlmultipoc.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <list>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <curl/curl.h>

struct Chunk
{
    std::string data;
    size_t size;
};

size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t totalSize = size * nmemb;
    Chunk *chunk = (Chunk *)userp;
    // chunk->data.append((char *)contents, totalSize);
    chunk->size += totalSize;
    // sleep for 0.1ms
    return totalSize;
}

CURL* createCurlHandle(long start, long end, Chunk* chunk, std::string url)
{
    CURL* curl = curl_easy_init();
    if (curl)
    {
        std::string range = std::to_string(start) + "-" + std::to_string(end);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, chunk);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L); // Enable SSL verification
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L); // Verify the host
    }
    return curl;
    
}

int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        std::cerr << "Usage: " << argv[0] << " <URL> <Chunk Size> <Total GB> <Number of Concurrent Connections>" << std::endl;
        return 1;
    }

    std::string url = argv[1];
    long chunkSize = std::stol(argv[2]) * 1024;
    long totalGB = std::stol(argv[3]);
    int numConnections = std::stoi(argv[4]);

    // Initialize CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Get the total size of the file
    CURL* curl = curl_easy_init();
    double fileSize = 0.0;
    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
        curl_easy_setopt(curl, CURLOPT_FILETIME, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L*1024L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // Enable SSL verification
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L); // Verify the host

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK)
        {
            curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &fileSize);
        }
        else
        {
            // get the http status code
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            std::cerr << "Failed to get file size: " << curl_easy_strerror(res) << " HTTP code: " << http_code << std::endl;
            return 1;
        }

        curl_easy_cleanup(curl);
    }

    auto numOfChunks = (__int64)totalGB*1024*1024*1024 / chunkSize;
    // if ((long)fileSize % chunkSize != 0)
    // {
    //     numOfChunks++;
    // }
    std::vector<Chunk> chunks;
    std::set<CURL *> handles;
    std::cout << "chunks: " << numOfChunks << std::endl;

    CURLM *multi_handle = curl_multi_init();

    // VRA options:
    // curl_multi_setopt(multi_handle, CURLMOPT_MAXCONNECTS, 0);
    // curl_multi_setopt(multi_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS, 1);
    // curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, 0L);

    // measure time taken to download
    clock_t start, end;
    start = clock();    

    int still_running = 0;
    int maxHandles = 0;
    int maxPolledFd = 0;
    do
    {
        // Add new connections
        while (handles.size() < numConnections && chunks.size() < numOfChunks)
        {
            // std::cout << "downloading chunk " << chunks.size() << "/" << numOfChunks << std::endl;
            // Create a new connection
            // long start = chunks.size() * chunkSize;
            // long end = (chunks.size() + 1) * chunkSize - 1;
            long start = 0;
            long end = chunkSize - 1;
            if (end >= fileSize) end = fileSize - 1;

            Chunk chunk;
            auto handle = createCurlHandle(start, end, &chunk, url);
            handles.insert(handle);
            curl_multi_add_handle(multi_handle, handle);
            chunks.push_back(chunk);
        }
        if (handles.size() > maxHandles)
        {
            maxHandles = handles.size();
        }

        // Perform connections
        CURLMcode mc = curl_multi_perform(multi_handle, &still_running);

        // Wait for activity, timeout or "nothing"
        int currentPolledFd = 0;
        if (still_running)
        {
            // mc = curl_multi_poll(multi_handle, NULL, 0, 1000, &currentPolledFd);
            mc = curl_multi_poll(multi_handle, NULL, 0, 1000, nullptr);
            if (currentPolledFd > maxPolledFd)
            {
                maxPolledFd = currentPolledFd;
            }
        }

        int handlesBeforeClean = handles.size();
        while (CURLMsg* msg = curl_multi_info_read(multi_handle, &still_running))
        {
            if (msg->msg == CURLMSG_DONE)
            {
                // std::cout << "http request done" << std::endl;
                CURL* handle = msg->easy_handle;
                CURLcode result = msg->data.result;
                if (result != CURLE_OK)
                {
                    std::cerr << "Download failed: " << curl_easy_strerror(result) << std::endl;
                }

                handles.erase(handle);
                curl_multi_remove_handle(multi_handle, handle);
                curl_easy_cleanup(handle);
            }
        }

        int handlesAfterClean = handles.size();
        // print handles before clean, after clean, and max on one line, but only if there isn't any change from previous iteration.
        // std::cout << "Handles before clean: " << handlesBeforeClean << " Handles after clean: " << handlesAfterClean << " Max handles: " << maxHandles << std::endl;
    }

    while (!(!still_running && handles.empty() && chunks.size() >= numOfChunks ));
    end = clock();
    double time_taken = double(end - start) / double(CLOCKS_PER_SEC);
    std::cout << "Time taken to download: " << time_taken << " seconds" << std::endl;
    // calculate download speed
    double downloadSpeed = (fileSize / 1024) / time_taken;
    std::cout << "Download speed: " << downloadSpeed << " KB/s" << std::endl;
    std::cout << "max polled fd: " << maxPolledFd << std::endl;


    curl_multi_cleanup(multi_handle);
    curl_global_cleanup();

    // Combine chunks
    std::string result;
    for (const auto &chunk : chunks)
    {
        result.append(chunk.data);
    }

    // Output the result
    std::cout << "Downloaded data: " << result << std::endl;

    return 0;
}

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
