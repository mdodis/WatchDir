#include <iostream>
#include <thread>
#include <locale>
#include <codecvt>
#include <chrono>
#include <algorithm>
#include <exception>
#include <Windows.h>

class WatcherException : std::exception {
public:
    std::string exp;
    WatcherException(std::string a) {
        exp = a;
    }
    const char* what() const noexcept { return exp.c_str(); }
};

struct Watcher {

    HANDLE dir_handle;

    Watcher(const std::string &watch_path);
    void start();

    static void watcher_proc(Watcher &self);
    static bool notify_changes(Watcher& self);
    
    std::string path;
    std::unique_ptr<std::thread> watcher_thread;

    static constexpr const UINT INFO_BUFFER_SIZE = 4096;
    bool is_running = false;

    DWORD LastErrCode = 0;
};


int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Too few arguments. Exiting." << std::endl;
        std::cerr << "Watches directory for file changes. This does not include changes to the directory itself!" << std::endl;
        std::cerr << "" << std::endl;
        std::cerr << "WatchDir [drive:][path]" << std::endl;
        return 1;
    }
    try {
        Watcher watcher(argv[1]);
        watcher.start();
        watcher.watcher_thread->join();
        
        std::cout << "Watcher stopped with error code: " << watcher.LastErrCode << std::endl;
    }
    catch (const WatcherException &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}


Watcher::Watcher(const std::string& watch_path) {
    dir_handle = CreateFile(
        watch_path.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL);

    if (dir_handle == INVALID_HANDLE_VALUE)
        throw WatcherException("Failed to open directory handle!");

    CloseHandle(dir_handle);
    path = watch_path;
}

void Watcher::start()
{
    watcher_thread = std::unique_ptr<std::thread>(new std::thread(Watcher::watcher_proc, std::ref(*this)));
}

void Watcher::watcher_proc(Watcher &self) {
    self.dir_handle = CreateFile(
        self.path.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL);

    self.is_running = true;

    while (notify_changes(self));

    self.LastErrCode = GetLastError();

    CloseHandle(self.dir_handle);

}

static std::string wchar_string(wchar_t* s, size_t bytes) {
    std::wstring wstring(s, bytes / 2);

    using convert_type = std::codecvt_utf8<wchar_t>;
    std::wstring_convert<convert_type, wchar_t> converter;

    return converter.to_bytes(wstring);
}

bool Watcher::notify_changes(Watcher& self) {
    DWORD bytes_read;
    FILE_NOTIFY_INFORMATION buffer[INFO_BUFFER_SIZE];
    BOOL result = ReadDirectoryChangesW(
        self.dir_handle,
        buffer,
        INFO_BUFFER_SIZE,
        TRUE,
        FILE_NOTIFY_CHANGE_SECURITY |
        FILE_NOTIFY_CHANGE_CREATION |
        FILE_NOTIFY_CHANGE_LAST_ACCESS |
        FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_ATTRIBUTES |
        FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_FILE_NAME,
        &bytes_read,
        0, 0);

    
    if (result == TRUE && bytes_read > 0) {

        bool keep_moving = false;
        FILE_NOTIFY_INFORMATION *notify_info = buffer;

        wchar_t *filename_ptr = (wchar_t*)&notify_info->FileName;

        std::string filename = wchar_string(filename_ptr, notify_info->FileNameLength);

        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::string ctime(std::ctime(&now));
        ctime.erase(std::remove(ctime.begin(), ctime.end(), '\n'), ctime.end());
    
        // Keep a copy of the previous event name, in case we get a
        // rename event. Hopefully those stay in order;
        //
        // Welp, that was quick! they do stay in order in that one
        // of the events apparently never gets sent!
        std::string renamed_other_name;

        do {
            switch (notify_info->Action) {
            case FILE_ACTION_ADDED:
                std::cout << ctime << "        Added           " << '\"' << filename << '\"' << std::endl;
                break;
            case FILE_ACTION_REMOVED:
                std::cout << ctime << "        Removed         " << '\"' << filename << '\"' << std::endl;
                break;
            case FILE_ACTION_MODIFIED:
                std::cout << ctime << "        Modified        " << '\"' << filename << '\"' << std::endl;
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                renamed_other_name = filename;
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:

                if (notify_info->NextEntryOffset != 0) {
                    FILE_NOTIFY_INFORMATION *new_notify_info = (FILE_NOTIFY_INFORMATION*)((char*)notify_info + notify_info->NextEntryOffset);
                    renamed_other_name = wchar_string((wchar_t*)&new_notify_info->FileName, new_notify_info->FileNameLength);
                    std::cout << ctime << "        Renamed         " << '\"' << filename << '\"' << " -> " << '\"' << renamed_other_name << '\"' << std::endl;

                    notify_info = new_notify_info;
                }
                else {
                    // With a huge enough buffer size, this shouldn't happen.
                    // It still technically could:
                    // for long paths or for a huge amount of changes since
                    // we search the modified event only in the current state
                    // of the buffer and not between calls of ReadDirectoryChanges.
                    std::cerr << "Detected rename action for new file: " << '\"' << filename << '\"' << "but no subsequent modification event was found" << std::endl;
                }

                break;
            default:
                break;
            }

            notify_info = (FILE_NOTIFY_INFORMATION*)((char*)notify_info + notify_info->NextEntryOffset);

            if (notify_info->NextEntryOffset == 0) keep_moving = false;
        } while (keep_moving);

        return true;
    } else {
        return false;
    }
}

