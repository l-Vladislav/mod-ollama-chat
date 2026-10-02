#ifndef OLLAMA_HTTP_CLIENT_H
#define OLLAMA_HTTP_CLIENT_H

#include <string>

class OllamaHttpClient
{
public:
    OllamaHttpClient();
    ~OllamaHttpClient();

    // Make HTTP POST request to Ollama API
    std::string Post(const std::string& url, const std::string& jsonData);

    // Make HTTP GET request (RSS/news feeds, etc.)
    std::string Get(const std::string& url);

    // Set timeout for requests (in seconds)
    void SetTimeout(int seconds);
    
    // Check if HTTP client is available
    bool IsAvailable() const;

private:
    int m_timeout;
    bool m_available;
};

#endif // OLLAMA_HTTP_CLIENT_H