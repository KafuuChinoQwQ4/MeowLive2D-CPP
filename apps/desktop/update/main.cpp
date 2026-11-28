#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

import meowlive.desktop.update.download;
import meowlive.desktop.update.manifest;
import meowlive.platform.windows.handle;

#ifndef MEOWLIVE_UPDATE_PUBLIC_KEY
#define MEOWLIVE_UPDATE_PUBLIC_KEY ""
#endif

#ifndef MEOWLIVE_VERSION
#define MEOWLIVE_VERSION "0.1.4"
#endif

namespace
{
    int install(const ::QStringList& args, const ::QByteArray& key)
    {
        if (args.size() != 7 || !::QDir::isAbsolutePath(args[3]) ||
            !::QDir::isAbsolutePath(args[4]) || !::QDir::isAbsolutePath(args[6]))
        {
            return 2;
        }
        const auto result = [&]() -> ::QString
        {
            bool valid_pid{};
            const auto pid = args[5].toUInt(&valid_pid);
            meowlive::platform::windows::UniqueHandle desktop(
                valid_pid ? ::OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr);
            if (!desktop && (!valid_pid || ::GetLastError() != ERROR_INVALID_PARAMETER))
            {
                return QStringLiteral("无法等待桌面端退出");
            }
            if (desktop)
            {
                const auto waited = ::WaitForSingleObject(desktop.get(), 30'000);
                desktop.reset();
                if (waited != WAIT_OBJECT_0)
                {
                    return QStringLiteral("等待桌面端退出超时");
                }
            }
            const ::QDir storage(args[3]);
            ::QLockFile guard(storage.filePath(QStringLiteral("update.lock")));
            guard.setStaleLockTime(0);
            if (!guard.tryLock())
            {
                return QStringLiteral("更新目录正被其他进程使用，请重试");
            }
            const auto manifest = meowlive::desktop::update::read_cached_manifest(
                args[2], QStringLiteral(MEOWLIVE_VERSION), key, storage);
            if (!manifest)
            {
                return manifest.error();
            }
            const auto installer = storage.filePath(manifest->installer);
            const auto native_path = ::QDir::toNativeSeparators(installer).toStdWString();
            const meowlive::platform::windows::UniqueHandle file(
                ::CreateFileW(native_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.get() == INVALID_HANDLE_VALUE)
            {
                return QStringLiteral("无法打开已下载的安装包");
            }
            if (!meowlive::desktop::update::installer_matches(installer, manifest->file))
            {
                return QStringLiteral("安装包校验失败，请重新下载");
            }
            ::QProcess setup;
            setup.setProgram(installer);
            setup.setWorkingDirectory(::QFileInfo(installer).absolutePath());
            // NSIS requires the unquoted /D path as the final raw command-line argument.
            setup.setNativeArguments(QStringLiteral("/D=") + ::QDir::toNativeSeparators(args[4]));
            const bool started = setup.startDetached();
            return started ? ::QString{} : QStringLiteral("无法启动安装程序");
        }();
        if (result.isEmpty())
        {
            return 0;
        }
        ::QSaveFile notice(args[6]);
        const auto bytes = result.toUtf8();
        if (notice.open(::QIODevice::WriteOnly) && notice.write(bytes) == bytes.size())
        {
            notice.commit();
        }
        return 1;
    }
}

int main(int argc, char** argv)
{
    const ::QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    const auto key = ::QByteArray::fromBase64(::QByteArray(MEOWLIVE_UPDATE_PUBLIC_KEY),
                                              ::QByteArray::AbortOnBase64DecodingErrors);
    if (arguments.value(1) == QStringLiteral("--install"))
    {
        return install(arguments, key);
    }
    if (arguments.size() != 3 || !::QDir::isAbsolutePath(arguments[2]))
    {
        ::QTextStream(stderr) << "Usage: meowlive-update RELEASE_TAG ABSOLUTE_CACHE_DIRECTORY\n";
        return 2;
    }
    const auto prepared = meowlive::desktop::update::prepare_update(
        arguments[1], QStringLiteral(MEOWLIVE_VERSION), key, ::QDir(arguments[2]));
    if (!prepared)
    {
        ::QTextStream(stderr) << prepared.error() << '\n';
        return 1;
    }
    ::QTextStream(stdout) << *prepared << '\n';
    return 0;
}
