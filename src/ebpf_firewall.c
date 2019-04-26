
#include "epbf_firewall.h"

#include <linux/bpf.h>
#include "bpf_helpers.h" //for ebpf wrapper functinos 
#include <stddef.h>
#include <stdbool.h> 
#include <linux/if_ether.h> 
#include <linux/ip.h>
//#include <netinet/ip.h>
//#include <arpa/inet.h>
#include <linux/tcp.h>
#include <linux/in.h>


#include <linux/pkt_cls.h>
#include <iproute2/bpf_elf.h>



#define SEC(NAME) __attribute__((section(NAME), used))

#define bpf_htons(x) ((__be16)___constant_swab16((x)))
//#define htonl(x) ((__be32)___constant_swab32((x)))



//////temp
#define TIMEOUT 0
//////


#ifndef lock_xadd
# define lock_xadd(ptr, val)              \
   ((void)__sync_fetch_and_add(ptr, val))
#endif


////////////
enum direction{ 
      INBOUND = 0x00000001,
      OUTBOUND = 0x00000002,
      DIRCTIONERR = 0x00000004
};

//2^n
enum filter_reult{
      PASS = 0x00000000,
      BLACKLIST = 0x00000001,
      PORTFORWARD = 0x00000002
};

/*
    To do
    - search other helper function, like BPF_FUNC_sk_redirect_map

*/


//first, ban the blacklist by IP (not consider the port)
struct bpf_map_def SEC("maps") blacklist = {
      .type        = BPF_MAP_TYPE_HASH,
      .key_size    = sizeof(__u32), //IP
      .value_size  = sizeof(__u32), 
      .max_entries = 0x1000000, //temp... did't decide the size
      .map_flags   = BPF_F_NO_PREALLOC,
};

struct bpf_map_def SEC("maps") port_forward_rule = {
      .type        = BPF_MAP_TYPE_HASH,
      .key_size    = sizeof(__u16), //outport
      .value_size  = sizeof(struct portforward_rule), 
      .max_entries = 0x10000, //temp... did't decide the size
      .map_flags   = BPF_F_NO_PREALLOC,
};

struct bpf_map_def SEC("maps") port_forward_table = {
      .type        = BPF_MAP_TYPE_HASH,
      .key_size    = sizeof(struct portforward_table_cinfo), //outport
      .value_size  = sizeof(struct portforward_table_sinfo), 
      .max_entries = 0x10000, //temp... did't decide the size
      .map_flags   = BPF_F_NO_PREALLOC,
};


struct bpf_map_def SEC("maps") mymac = {
      .type        = BPF_MAP_TYPE_HASH,
      .key_size    = sizeof(__u8),
      .value_size  = sizeof(struct mac), 
      .max_entries = 0x1,
      .map_flags   = BPF_F_NO_PREALLOC,
};


struct bpf_elf_map SEC("maps") result = {
      .type        = BPF_MAP_TYPE_HASH,
      .size_key    = sizeof(__u32),
      .size_value  = sizeof(struct test),
      .pinning        = PIN_GLOBAL_NS, 
      .max_elem       = 100,
};


struct bpf_elf_map SEC("maps") counter = {
      .type        = BPF_MAP_TYPE_HASH,
      .size_key    = sizeof(__u32),
      .size_value  = sizeof(__u32), 
      .pinning        = PIN_GLOBAL_NS, 
      .max_elem       = 1,
};


static __always_inline int process_blacklist(__u32 saddr)
{
      __u32 *value = bpf_map_lookup_elem(&blacklist, &saddr);
      
      if(value != NULL)
            return BLACKLIST;
      else
            return PASS;   
}

static __always_inline int redirect_tcp(struct iphdr *ip, struct tcphdr * tcp, __u32 direction,  void *data_end )
{
      struct portforward_rule * rule = NULL;
      __u16 rule_key;
      struct portforward_table_cinfo cinfo = {};
      struct portforward_table_sinfo *sinfo = NULL;
      struct portforward_table_sinfo sinfo_reg = {};
     
      //out -> in 
      if(direction == INBOUND) {

            rule_key = tcp->dest;
            

            rule = bpf_map_lookup_elem(&port_forward_rule, &rule_key);
            
            if(rule == NULL)
                  return PASS;

            cinfo.ip = ip->saddr;
            cinfo.port = tcp->source;
      
            sinfo = bpf_map_lookup_elem(&port_forward_table, &cinfo);

            //if it was not connected already
            if(sinfo == NULL) {
                  //add it to portfoward table
                  sinfo_reg.outport = rule->outport;
                  sinfo_reg.inport = rule->inport;
                  sinfo = &sinfo_reg;
            }
      
            //change packet
            tcp->dest = sinfo->inport;
      }
      //in -> out
      else if (direction == OUTBOUND) {
                  
            

            cinfo.ip = ip->daddr;
            cinfo.port = tcp->dest;
      
            sinfo = bpf_map_lookup_elem(&port_forward_table, &cinfo);

            //if it was not connected already
            if(sinfo == NULL) {
                  return BLACKLIST;
            }
            //change packet
            tcp->source = sinfo->outport;
      }

      //update timeout
      sinfo->timeout = TIMEOUT;
      
      ///////temp
      bpf_map_update_elem(&port_forward_table, &cinfo, sinfo, BPF_ANY);

      return PORTFORWARD;
}


static __always_inline int process_redirect(struct iphdr *ip, __u32 direction, void *data_end)
{
      
      if(ip == NULL)
            return PASS;
            

      switch(ip->protocol)
      {
      
      case IPPROTO_TCP : {

            __u32 iplen = (__u32)(ip->ihl) * 4;
            struct tcphdr * tcp;
            //tcp = ip + 1;
            tcp = (struct tcphdr *)((__u8 *)ip + iplen);
      
            if (tcp + 1 > data_end)
	            return BLACKLIST;////temp
            
            return redirect_tcp(ip, tcp, direction, data_end); 
            
            break;
            }
      default:
            /////temp
            return PASS;
            break;
      } //switch(ip->protocol)
      
      
 
      
}

static __always_inline int filter_ipv4(struct iphdr *ip, __u32 direction, void *data_end )
{
      int filter_flag = 0;
      int rule = XDP_PASS;

      filter_flag |= process_blacklist(ip->saddr);

      //drop first
      if(filter_flag & BLACKLIST)
      {
            rule = XDP_DROP;
            goto filter_ipv4_end;
      }

      filter_flag |= process_redirect(ip, direction, data_end);
      if(filter_flag & PORTFORWARD)
      {
            rule = XDP_PASS; //????
            goto filter_ipv4_end;
      }

      //////temp
      if(filter_flag & BLACKLIST)
      {
            rule = XDP_DROP;
            goto filter_ipv4_end;
      }
      
filter_ipv4_end :

      return rule;
}

static __always_inline int bpf_memcmp(void * s1, void * s2, __s32 n)
{
      for(__s32 i = 0; i< n; i++) {

            if(((__u8 *)s1)[i] != ((__u8 *)s2)[i])
                  return 1;
      }

      return 0;
}

static __always_inline int get_direction(struct ethhdr *eth, void *data_end)
{
      __u8 key = 0;
      struct mac *macaddr = bpf_map_lookup_elem(&mymac, &key);

      if(macaddr==NULL)
            return DIRCTIONERR;

      if(!bpf_memcmp(eth->h_dest, macaddr, 6))
            return INBOUND;
      else
           return DIRCTIONERR;

}

static __always_inline int get_filter_result(void *data, void *data_end)
{
      struct ethhdr *eth = data;
      __u16 eth_proto = eth->h_proto;
      
      __u32 direction = get_direction(eth, data_end);
      
      if(direction == DIRCTIONERR)
            return XDP_DROP;
      
      //process filtering
	if (eth_proto == bpf_htons(ETH_P_IP)){

            struct iphdr *ip = data + sizeof(struct ethhdr);
            if (ip + 1 > data_end)
		      return XDP_DROP;

            return filter_ipv4(ip, direction, data_end);
      }
	else
		return XDP_PASS;
}


//main
SEC("xdp")
int xdp_filter(struct xdp_md *xdp)
{	
      void *data_end = (void *)(long)xdp->data_end;
	void *data = (void *)(long)xdp->data;
	struct ethhdr *eth = data;

	if (eth + 1 > data_end)
	      return XDP_DROP;
      else
            return get_filter_result(data, data_end);      
}




#ifndef BPF_FUNC
# define BPF_FUNC(NAME, ...)              \
   (*NAME)(__VA_ARGS__) = (void *)BPF_FUNC_##NAME
#endif

static void *BPF_FUNC(map_lookup_elem, void *map, const void *key);

static __always_inline int account_data(struct __sk_buff *skb, __u32 dir)
{
      __u32 *bytes;

 
      __u32 *p_counter = NULL;

   
      __u8 *ptr = (__u8 *)(long)skb->data;
      __u32 k = 0;
      p_counter = bpf_map_lookup_elem(&counter, &k);
      

      if(p_counter == NULL)
            return TC_ACT_SHOT;

      __u32 temp = *p_counter;


      struct test t = {};
      
      if(skb != NULL) {
            t.addr_dest[0] = (__u8)(skb->len);
           
            // t.addr_dest[2] = ptr[2];
            // t.addr_dest[3] = ptr[3];
            // t.addr_dest[4] = ptr[4];
            // t.addr_dest[5] = ptr[5];
      }

      temp +=1;

      bpf_map_update_elem(&result, &temp, &t, BPF_ANY);

      //update counter
      bpf_map_update_elem(&counter, &k, &temp, BPF_ANY);

/*
      bytes = map_lookup_elem(&acc_map, &dir);
      if (bytes)
            lock_xadd(bytes, skb->len);
*/
      return TC_ACT_OK;
}



SEC("egress")
int tc_egress(struct __sk_buff *skb)
{
     
      return account_data(skb, 1);
}




char _license[] SEC("license") = "GPL";
 
