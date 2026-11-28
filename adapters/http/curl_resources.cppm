module;

#include <curl/curl.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.http.curl_resources;

import meowlive.standard;

export namespace meowlive::adapters::http
{
    ::CURLcode check_curl_memory(::CURLcode result)
    {
        if (result == CURLE_OUT_OF_MEMORY)
        {
            throw std::bad_alloc{};
        }
        return result;
    }

    ::CURLMcode check_curl_memory(::CURLMcode result)
    {
        if (result == CURLM_OUT_OF_MEMORY)
        {
            throw std::bad_alloc{};
        }
        return result;
    }

    ::CURLUcode check_curl_memory(::CURLUcode result)
    {
        if (result == CURLUE_OUT_OF_MEMORY)
        {
            throw std::bad_alloc{};
        }
        return result;
    }
}
