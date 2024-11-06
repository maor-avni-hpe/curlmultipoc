// curlmultipoc.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <list>
#include <set>
#include <string>
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
    chunk->data.append((char *)contents, totalSize);
    chunk->size += totalSize;
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
    }
    return curl;
    
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        std::cerr << "Usage: " << argv[0] << " <URL> <Chunk Size> <Number of Concurrent Connections>" << std::endl;
        return 1;
    }

    std::string url = argv[1];
    long chunkSize = std::stol(argv[2]) * 1024 * 1024;
    int numConnections = std::stoi(argv[3]);

    // Initialize CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Get the total size of the file
    CURL *curl = curl_easy_init();
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

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK)
        {
            curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &fileSize);
        }
        else
        {
            std::cerr << "Failed to get file size: " << curl_easy_strerror(res) << std::endl;
            return 1;
        }

        curl_easy_cleanup(curl);
    }

    auto numOfChunks = (long)fileSize / chunkSize;
    if ((long)fileSize % chunkSize != 0)
    {
        numOfChunks++;
    }
    std::vector<Chunk> chunks;
    std::set<CURL *> handles;
    std::cout << handles.size() << std::endl;

    CURLM *multi_handle = curl_multi_init();

    // for (int i = 0; i < numConnections; ++i)
    // {
    //     long start = i * chunkSize;
    //     long end = (i + 1) * chunkSize - 1;
    //     if (end >= fileSize) end = fileSize - 1;
    //
    //     handles[i] = curl_easy_init();
    //     std::string range = std::to_string(start) + "-" + std::to_string(end);
    //     curl_easy_setopt(handles[i], CURLOPT_URL, url.c_str());
    //     curl_easy_setopt(handles[i], CURLOPT_WRITEFUNCTION, WriteCallback);
    //     curl_easy_setopt(handles[i], CURLOPT_WRITEDATA, &chunks[i]);
    //     curl_easy_setopt(handles[i], CURLOPT_RANGE, range.c_str());
    //
    //     curl_multi_add_handle(multi_handle, handles[i]);
    // }

    int still_running = 0;
    do
    {
        // Add new connections
        while (handles.size() < numConnections && chunks.size() < numOfChunks)
        {
            std::cout << "downloading chunk " << chunks.size() << "/" << numOfChunks << std::endl;
            // Create a new connection
            long start = chunks.size() * chunkSize;
            long end = (chunks.size() + 1) * chunkSize - 1;
            if (end >= fileSize) end = fileSize - 1;

            Chunk chunk;
            auto handle = createCurlHandle(start, end, &chunk, url);
            handles.insert(handle);
            curl_multi_add_handle(multi_handle, handle);
            chunks.push_back(chunk);
        }

        // Perform connections
        CURLMcode mc = curl_multi_perform(multi_handle, &still_running);

        // Wait for activity, timeout or "nothing"
        if (still_running)
        {
            mc = curl_multi_poll(multi_handle, NULL, 0, 1000, NULL);
        }

        while (CURLMsg* msg = curl_multi_info_read(multi_handle, &still_running))
        {
            if (msg->msg == CURLMSG_DONE)
            {
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
    }
    while (!(!still_running && handles.empty() && chunks.size() >= numOfChunks ));

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
