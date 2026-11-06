# SPDX-License-Identifier: GPL-2.0-or-later
#
# wallpaperx_write_build_id(DIR): writes DIR/build-id, a hash of every file under DIR (its path, and
# its content or link target), so first-run setup can tell whether what it installed from DIR is what
# DIR holds now. Bytes that do not change keep the id.

function(wallpaperx_write_build_id dir)
    file(GLOB_RECURSE entries LIST_DIRECTORIES false RELATIVE "${dir}" "${dir}/*")
    list(REMOVE_ITEM entries build-id)
    list(SORT entries)
    set(manifest "")
    foreach(entry IN LISTS entries)
        if(IS_SYMLINK "${dir}/${entry}")
            file(READ_SYMLINK "${dir}/${entry}" target)
            string(APPEND manifest "${entry}\n-> ${target}\n")
        else()
            file(SHA256 "${dir}/${entry}" sum)
            string(APPEND manifest "${entry}\n${sum}\n")
        endif()
    endforeach()
    string(SHA256 id "${manifest}")
    string(SUBSTRING "${id}" 0 16 id)
    file(WRITE "${dir}/build-id" "${id}\n")
endfunction()
