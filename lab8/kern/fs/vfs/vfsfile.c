#include <defs.h>
#include <string.h>
#include <vfs.h>
#include <inode.h>
#include <unistd.h>
#include <error.h>
#include <assert.h>


// open file in vfs, get/create inode for file with filename path.
// 负责把“路径 + flags”转换成一个打开好的 inode *node （必要时还会创建/截断），
// 并把这个 inode 返回给上层（ file_open ）。
int
vfs_open(char *path, uint32_t open_flags, struct inode **node_store) {
    //（1）做 flags 合法性检查（O_TRUNC必须可写）
    bool can_write = 0;
    switch (open_flags & O_ACCMODE) {
    case O_RDONLY:
        break;
    case O_WRONLY:
    case O_RDWR:
        can_write = 1;
        break;
    default:
        return -E_INVAL;
    }

    if (open_flags & O_TRUNC) {//如果指定了 O_TRUNC 标志说明要截断文件
        if (!can_write) {//此时如果文件只读则不能截断
            return -E_INVAL;//返回错误码 -E_INVAL
        }
    }

    int ret; 
    struct inode *node;
    bool excl = (open_flags & O_EXCL) != 0;//如果指定了 O_EXCL 标志说明要检查文件是否存在
    bool create = (open_flags & O_CREAT) != 0;//如果指定了 O_CREAT 标志说明要创建文件
    //（2）根据路径查找 inode
    ret = vfs_lookup(path, &node);//根据路径查找 inode
    //（3）“不存在就创建”： vfs_lookup_parent + vop_create
    if (ret != 0) {//如果失败
        if (ret == -16 && (create)) {//如果返回 -16 说明文件不存在
            char *name;
            struct inode *dir;
            if ((ret = vfs_lookup_parent(path, &dir, &name)) != 0) {//找到父目录inode以及文件名
                return ret;
            }
            ret = vop_create(dir, name, excl, &node);//调用具体文件体统的创建文件函数在父目录创建文件 
        } else return ret;
    } else if (excl && create) {//ret=0说明存在文件但还指定了文件不存在或者需要创建文件
        return -E_EXISTS;
    }
    assert(node != NULL);
    //（4）调用具体文件系统的打开文件函数打开文件 通过node 指针
    if ((ret = vop_open(node, open_flags)) != 0) {//失败
        vop_ref_dec(node);//回收引用 减少引用次数
        return ret;
    }
    //（5）必要时截断： vop_truncate(node, 0)
    vop_open_inc(node);//增加引用次数 说明有一个打开操作
    if (open_flags & O_TRUNC || create) {
        if ((ret = vop_truncate(node, 0)) != 0) {//如果截断文件失败
            vop_open_dec(node);
            vop_ref_dec(node);
            return ret;
        }
    }
    *node_store = node;//（6）成功打开文件后将 inode 指针返回给上层
    return 0;
}

// close file in vfs
int
vfs_close(struct inode *node) {
    vop_open_dec(node);
    vop_ref_dec(node);
    return 0;
}

// unimplement
int
vfs_unlink(char *path) {
    return -E_UNIMP;
}

// unimplement
int
vfs_rename(char *old_path, char *new_path) {
    return -E_UNIMP;
}

// unimplement
int
vfs_link(char *old_path, char *new_path) {
    return -E_UNIMP;
}

// unimplement
int
vfs_symlink(char *old_path, char *new_path) {
    return -E_UNIMP;
}

// unimplement
int
vfs_readlink(char *path, struct iobuf *iob) {
    return -E_UNIMP;
}

// unimplement
int
vfs_mkdir(char *path){
    return -E_UNIMP;
}
