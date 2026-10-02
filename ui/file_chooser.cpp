#include <vector>
#include <string>
#include <ctype.h>

extern "C" {
#include "ff.h"
}

#include "file_chooser.h"
#include "overlay.h"
#include "utils.h"

#define PAGESIZE 22
#define TOPLINE 2

// void FileChooser::set_fs(FATFS *fs) {
//     this->fs = fs;
// }

bool FileChooser::accepts_file(const string &name) const {
    if (extensions.empty())
        return true;
    for (const string &extension : extensions) {
        if (name.size() < extension.size())
            continue;
        const size_t offset = name.size() - extension.size();
        bool match = true;
        for (size_t index = 0; index < extension.size(); index++) {
            if (tolower(static_cast<unsigned char>(name[offset + index])) !=
                tolower(static_cast<unsigned char>(extension[index]))) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

// list files in dir, starting from number `start` and return at most `len` files. `count` is set to total number of files.
bool FileChooser::list_files(string dir, vector<FileEntry> &files, int start, int len, int *count) {
    DIR d;
    
    if (f_opendir(&d, dir.c_str()) != 0)
        return false;
    files.clear();

    // ".." entry at the top
    if (start == 0 && len > 0) {
        files.push_back({dir == rootdir ? msg_return : "..", true});
    }
    if (start > 0) start--;
    *count = 1;

    FILINFO fno;
    while (f_readdir(&d, &fno) == FR_OK && fno.fname[0] != 0) {
        const bool is_dir = (fno.fattrib & AM_DIR) != 0;
        if (!is_dir && !accepts_file(fno.fname))
            continue;
        (*count)++;
        if (start > 0) {
            start--;
        } else if (files.size() < len)
            files.push_back({fno.fname, is_dir});
    }
    f_closedir(&d);
    return true;
}

bool FileChooser::choose_file(string &res) {
    int page = 0, pages, total;
    int active = 0;
    vector<FileEntry> files;

    while (1) {
        overlay_clear();
        bool r = list_files(this->curdir, files, page*PAGESIZE, PAGESIZE, &total);
        if (r) {
            if (!title.empty()) {
                overlay_cursor(0, 0);
                overlay_printf("%s", title.c_str());
            }
            pages = (total+PAGESIZE-1) / PAGESIZE;
            overlay_status("Page ");
            overlay_printf("%d/%d", page+1, pages);
            if (active > files.size()-1)
                active = files.size()-1;
            for (int i = 0; i < PAGESIZE; i++) {
                int idx = page*PAGESIZE + i;
                overlay_cursor(2, i+TOPLINE);
                if (i < files.size()) {
                    overlay_printf("%s", files[i].name.c_str());
                    if (idx != 0 && files[i].is_dir)
                        overlay_printf("/");
                }
            }
            delay(300);
            while (1) {
                int r = joy_choice(TOPLINE, files.size(), &active, OSD_KEY_CODE);
                if (r == 1 || r == 4) {
                    if (curdir == rootdir && page == 0 && active == 0) {// return to main menu
                        return false;
                    } else if (files[active].is_dir) {          // enter dir
                        if (page == 0 && active == 0) {         // return to parent dir
                            curdir = curdir.substr(0, curdir.find_last_of("/"));
                        } else {								// enter sub dir
                            curdir += "/" + files[active].name;
                        }
                        active = 0;
                        page = 0;
                        break;
                    } else {                                    // file chosen
                        res = curdir + "/" + files[active].name;
                        return true;
                    }
                } else if (r == 2 && page < pages-1) {
                    page++;
                    break;
                } else if (r == 3 && page > 0) {
                    page--;
                    break;
                }
                delay(10);
            }
        } else {
            overlay_status("Error opening directory");
            overlay_printf(" %d", r);
            return false;
        }
    }        
}
