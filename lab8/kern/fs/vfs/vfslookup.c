#include <defs.h>
#include <string.h>
#include <vfs.h>
#include <inode.h>
#include <error.h>
#include <assert.h>

/*
 * get_device- Common code to pull the device name, if any, off the front of a
 *             path and choose the inode to begin the name lookup relative to.
 */

static int //把路径拆成「起始 inode + 子路径」
get_device(char *path, char **subpath, struct inode **node_store) {
    int i, slash = -1, colon = -1;
    for (i = 0; path[i] != '\0'; i ++) {
        if (path[i] == ':') { colon = i; break; }
        if (path[i] == '/') { slash = i; break; }
    }
    if (colon < 0 && slash != 0) {//情况 A：相对路径/裸文件名
        /* *
         * No colon before a slash, so no device name specified, and the slash isn't leading
         * or is also absent, so this is a relative path or just a bare filename. Start from
         * the current directory, and use the whole thing as the subpath.
         * */
        *subpath = path;
        return vfs_get_curdir(node_store);//起点是当前目录
    }
    if (colon > 0) {//情况 B：显式设备名 device:path 或 device:/path
        /* device:path - get root of device's filesystem */
        path[colon] = '\0';

        /* device:/path - skip slash, treat as device:path */
        while (path[++ colon] == '/');
        *subpath = path + colon;//子路径
        return vfs_get_root(path, node_store);//起点是设备根目录
    }
    //情况 C： /path 或 :path
    /* *
     * we have either /path or :path
     * /path is a path relative to the root of the "boot filesystem"
     * :path is a path relative to the root of the current filesystem
     * */
    int ret;
    if (*path == '/') {//情况 C1： /path
        if ((ret = vfs_get_bootfs(node_store)) != 0) {//从 boot filesystem 的根开始
            return ret;
        }
    }
    else {//情况 C2： :path
        assert(*path == ':');
        struct inode *node;
        if ((ret = vfs_get_curdir(&node)) != 0) {//从“当前文件系统”的根开始
            return ret;
        }
        /* The current directory may not be a device, so it must have a fs. */
        assert(node->in_fs != NULL);
        *node_store = fsop_get_root(node->in_fs);
        vop_ref_dec(node);
    }

    /* ///... or :/... */
    while (*(++ path) == '/');
    *subpath = path;
    return 0;
}

/*
 * vfs_lookup - get the inode according to the path filename
 */
int //vfs_lookup：根据 path 找到最终 inode
vfs_lookup(char *path, struct inode **node_store) {
    int ret;
    struct inode *node;
    //（1）get_device：把路径拆成「起始 inode + 子路径」拿到起始 inode
    if ((ret = get_device(path, &path, &node)) != 0) {
        return ret;
    }
    //（2）如果子路径非空，调用文件系统实现的 vop_lookup(node, path, node_store)
    if (*path != '\0') {
        ret = vop_lookup(node, path, node_store);
        vop_ref_dec(node);
        return ret;
    }
    //（3）如果子路径为空：说明用户传的是“某个根本身”，直接返回起始 inode
    *node_store = node;
    return 0;
}

/*
 * vfs_lookup_parent - Name-to-vnode translation.
 *  (In BSD, both of these are subsumed by namei().)
 */
int
vfs_lookup_parent(char *path, struct inode **node_store, char **endp){
    int ret;
    struct inode *node;
    if ((ret = get_device(path, &path, &node)) != 0) {
        return ret;
    }
    *endp = path;
    *node_store = node;
    return 0;
}
