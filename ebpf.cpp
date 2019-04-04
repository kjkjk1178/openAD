/*  Copyright(c) 2019 kjkjk1178.
 *  Copyright(c) 2017 Jesper Dangaard Brouer, Red Hat, Inc.
 *  Copyright(c) 2017 Andy Gospodarek, Broadcom Limited, Inc.
 */
#include "ebpf.h"
#include "bpf.h"
#include "libbpf.h"
#include <net/if.h>
#include <string.h>
#include <iostream>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>
#include "bpf_load.h"
#include <unistd.h>
#include <sys/statfs.h>
#include <libgen.h>

#ifndef BPF_FS_MAGIC
# define BPF_FS_MAGIC   0xcafe4a11
#endif

using namespace std;


void EBPFLoader::PreloadMapsViaFs(struct bpf_map_data *map_data, int32_t mapnum)
{
    string file;

    map_index.insert(make_pair(map_data->name, mapnum));

    file = map_path.find(map_data->name)->second;

    //if already exoirted    
    if (OpenExportedMap(file, map_data)) {

        map_data->fd = fd_map_exported.find(map_data->name)->second;
        map_exported.insert(make_pair(map_data->name, true));

    } 
    else 
        map_exported.insert(make_pair(map_data->name, false));
    
}

//open exported map
bool EBPFSuper::OpenExportedMap(string path, struct bpf_map_data *map_data)
{
    if (bpf_fs_check_path(path) < 0) {
        exit(-1);
    }

    int32_t fd = bpf_obj_get(path.c_str());

    if (fd < 0) {
        printf("ERR: Failed to open bpf map file:%s err(%d):%s\n",
            path, errno, strerror(errno));
        return false;
    }
    
    fd_map_exported.insert(make_pair(map_data->name, fd));

    return true;
}




int EBPFSuper::bpf_fs_check_path(string path)
{
    struct statfs st_fs;
    char *dname, *dir;
    int err = 0;

    if (path == "")
        return -EINVAL;

    dname = strdup(path.c_str());
    if (dname == NULL)
        return -ENOMEM;

    dir = dirname(dname);
    if (statfs(dir, &st_fs)) {
        fprintf(stderr, "ERR: failed to statfs %s: (%d)%s\n",
            dir, errno, strerror(errno));
        err = -errno;
    }
    free(dname);

    if (!err && st_fs.f_type != BPF_FS_MAGIC) {
        fprintf(stderr,
                "ERR: specified path %s is not on BPF FS\n\n"
                " You need to mount the BPF filesystem type like:\n"
                "  mount -t bpf bpf /sys/fs/bpf/\n\n",
                path);
        err = -EINVAL;
    }

  return err;
}





void EBPFLoader::chown_maps(uid_t owner, gid_t group)
{
    map<string, bool>::iterator iter;
    for(iter = map_exported.begin(); iter!=map_exported.end(); iter ++)
    {   
        if(iter->second)
            continue;
     
        int32_t index = map_index.find(iter->first)->second;
        string path = map_path.find(iter->first)->second;

        if (chown(path.c_str(), owner, group) < 0)
            fprintf(stderr,"WARN: Cannot chown file:%s err(%d):%s\n",
	                path, errno, strerror(errno));
    }
}








EBPFLoader::EBPFLoader(string filepath, string interface)
{
    filepath_ebpf = filepath;
    net_interface = interface;
}

int32_t EBPFLoader::Load()
{

	uid_t owner = -1; /* -1 result in no-change of owner */
	gid_t group = -1;
	return 0;

    //get network interface number
    uint32_t ifindex = if_nametoindex(net_interface.c_str());
    if (ifindex == 0) {
        return -1;
    }

    if (ifindex == -1) {
	   return -1;
	}
		
	if (load_bpf_file_fixup_map(filepath_ebpf.c_str(), EBPFLoader::PreloadMapsViaFs)) {
	  fprintf(stderr, "Error in load_bpf_file_fixup_map(): %s", bpf_log_buf);
		return 1;
	}

	if (!prog_fd[0]) {
		printf("load_bpf_file: %s\n", strerror(errno));
		return 1;
	}

	ExportMaps();

	if (owner >= 0)
	  chown_maps(owner, group);

	if (bpf_set_link_xdp_fd(ifindex, prog_fd[0], 0) < 0) {
		printf("link set xdp fd failed\n");
		return 1;
	}



}


void EBPFLoader::ExportMaps(void)
{
    map<string, bool>::iterator iter;
    for(iter = map_exported.begin(); iter!=map_exported.end(); iter ++)
    {   
        if(iter->second)
            continue;
     
        int32_t index = map_index.find(iter->first)->second;
        string path = map_path.find(iter->first)->second;

        if (bpf_obj_pin(map_fd[index], path.c_str()) != 0) {
            fprintf(stderr, "ERR: Cannot pin map(%s) file:%s err(%d):%s\n",
            map_data[index].name, path, errno, strerror(errno));
        }
    }
}



int main()
{
   /* EBPFLoader e;
    e.Open_bpf_map(file_blacklist);
    
        map_path.insert(make_pair((int32_t)MAPNUM::BLACKLIST, "/sys/fs/bpf/blacklist"));
    char * ip_string = "52.79.102.173";
   
	int res = add_blacklist(e.fd_blacklist, ip_string);*/
//	close(fd_blacklist);

    EBPFLoader e("", "");
    //e.Load();
    string a = "";
    //std::cout << "hi" << e.OpenExportedMapBlacklist("") << "\n";
    /*char * ip_string = "52.79.102.173";
   
	int res = add_blacklist(e.fd_blacklist, ip_string);*/

}
