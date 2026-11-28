export module meowlive.server.bootstrap;

export namespace meowlive::server
{
#if defined(_WIN32)
    int run(int count, wchar_t* const* arguments);
#else
    int run(int count, char* const* arguments);
#endif
}
