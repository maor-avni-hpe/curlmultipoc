#include <iostream>
#include <list>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <curl/curl.h>

struct Chunk
{
    std::string data;
    size_t size;
};

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t totalSize = size * nmemb;
    Chunk* chunk = (Chunk*)userp;
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

int main(int argc, char* argv[])
{
    if (argc != 6)
    {
        std::cerr << "Usage: " << argv[0] << " <URL> <Chunk Size> <Total GB> <Batch Size> <Number of Concurrent Connections>" << std::endl;
        return 1;
    }

    std::string url = argv[1];
    long chunkSize = std::stol(argv[2]) * 1024;
    long totalGB = std::stol(argv[3]);
    int batchSize = std::stoi(argv[4]);
    int numConnections = std::stoi(argv[5]);

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
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024L);
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

    long long numOfChunks = (long long)totalGB * 1024 * 1024 * 1024 / chunkSize;
    long long totalBatches;
    if (batchSize == 0)
    {
        batchSize = numOfChunks;
        totalBatches = 1;
    }
    else
        totalBatches = numOfChunks / batchSize;

    std::vector<Chunk> chunks;
    std::map<CURL*, std::chrono::steady_clock::time_point> handles;
    std::cout << "chunks: " << numOfChunks << std::endl;

    CURLM* multi_handle = curl_multi_init();

    // curl_multi_setopt(multi_handle, CURLMOPT_MAXCONNECTS, 0);
    // curl_multi_setopt(multi_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS, 1);
    // curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, 0L);

    // measure time taken to download not using clock
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    // build a histogram of the time taken to poll
    std::vector<double> pollTimes;

    // build a histogram of the time taken to download each handle
    std::vector<double> downloadTimes;

    int still_running = 0;
    int maxHandles = 0;
    int maxPolledFd = 0;
    long long currentBatch = 0;
    do
    {
        do
        {
            // Add new connections
            while (handles.size() < numConnections && chunks.size() < batchSize)
            {
                long start = 0;
                long end = chunkSize - 1;
                if (end >= fileSize) end = fileSize - 1;

                Chunk chunk;
                auto handle = createCurlHandle(start, end, &chunk, url);
                handles.insert(std::pair<CURL*, std::chrono::steady_clock::time_point>(handle, std::chrono::steady_clock::now()));
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
                // measure call time in milliseconds using chrono
                std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
                mc = curl_multi_poll(multi_handle, NULL, 0, 1000, nullptr);
                std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
                double time_taken = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                pollTimes.push_back(time_taken);

                //if (time_taken > 0.1)
                    //std::cout << "Time taken to poll: " << time_taken << " seconds. Still running: " << still_running << std::endl;

                if (mc != CURLM_OK)
                {
                    std::cerr << "curl_multi_poll failed: " << curl_multi_strerror(mc) << std::endl;
                }

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

                    // measure time taken to download not using clock
                    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
                    double time_taken = std::chrono::duration_cast<std::chrono::milliseconds>(end - handles[handle]).count();
                    downloadTimes.push_back(time_taken);

                    handles.erase(handle);
                    curl_multi_remove_handle(multi_handle, handle);
                    curl_easy_cleanup(handle);
                }
            }

            int handlesAfterClean = handles.size();
            // print handles before clean, after clean, and max on one line, but only if there isn't any change from previous iteration.
            // std::cout << "Handles before clean: " << handlesBeforeClean << " Handles after clean: " << handlesAfterClean << " Max handles: " << maxHandles << std::endl;
        } while (!(!still_running && handles.empty() && chunks.size() >= batchSize));
        ++currentBatch;
        chunks.clear();
    } while (currentBatch < totalBatches);

    // measure time taken to download not using clock
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::cout << "Time taken to download: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << " milliseconds" << std::endl;

    // calculate data rate
    double dataRate = (double)totalGB / std::chrono::duration_cast<std::chrono::seconds>(end - begin).count();
    std::cout << "Data rate: " << dataRate << " GB/s" << std::endl;

    std::cout << "max polled fd: " << maxPolledFd << std::endl;

    curl_multi_cleanup(multi_handle);
    curl_global_cleanup();

    std::map<int, int> pollTimeHistogram;
    std::map<int, int> downloadTimeHistogram;

    for (auto time : pollTimes)
    {
        int timeInt = (int)time;
        if (pollTimeHistogram.find(timeInt) == pollTimeHistogram.end())
        {
            pollTimeHistogram[timeInt] = 1;
        }
        else
        {
            pollTimeHistogram[timeInt]++;
        }
    }

    for (auto time : downloadTimes)
    {
        int timeInt = (int)time;
        if (downloadTimeHistogram.find(timeInt) == downloadTimeHistogram.end())
        {
            downloadTimeHistogram[timeInt] = 1;
        }
        else
        {
            downloadTimeHistogram[timeInt]++;
        }
    }

    std::cout << "Poll time histogram: " << std::endl;
    for (auto time : pollTimeHistogram)
    {
        std::cout << time.first << " " << time.second << std::endl;
    }

    std::cout << "Download time histogram: " << std::endl;
    for (auto time : downloadTimeHistogram)
    {
        std::cout << time.first << " " << time.second << std::endl;
    }

    return 0;
}

