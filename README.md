# 3-Huqy518
3-Huqy518 created by GitHub Classroom
### 通过将文件系统当作循环链表，每次循环查找没有使用过的block然后逐一mmap来分配内存
### 通过content[MAX_BLOCK_NUM]来查找block所在位置然后逐一munmap删除。
### 最大支持文件32K * 10240。
### 每个保存文件信息的node存放在一个页里。
### 读取时，先判断读取块数和块内偏移，然后分别读取块内偏移以及整块
