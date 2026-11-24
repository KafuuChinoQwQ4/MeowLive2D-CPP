import meowlive.server.bootstrap;

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[])
#else
int main(int argc, char* argv[])
#endif
{
    return meowlive::server::run(argc, argv);
}
