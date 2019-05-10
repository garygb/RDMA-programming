#include <infiniband/verbs.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <time.h>

// MR的大小
#define REGION_SIZE 0x1800
// minimum CQ size
#define CQ_SIZE 0x100
//maximum number of outstanding WQ that can be posted to SQ
#define MAX_NUM_SENDS 0x10
#define MAX_GATHER_ENTRIES 2
#define MAX_SCATTER_ENTRIES 2

#define TEXT_MSG "Hello UD :)"

#define WELL_KNOWN_QKEY 0x11111111


// for time testing
#define MEM_POOL_SIZE 500 //单位:MB
#define BLOCK_SIZE 1024 //一次读取文件内容到MEM_POOL的单位大小
#define DEBUG

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s send packets to remote\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -d, --dev-name=<dev>   use  device <dev>)\n");
	printf("  -i, --dev_port=<port>  use port <port> of device (default 1)\n");
	printf("  -l, --dest_lid=<lid>  use lid as remote lid on destination port (Infiniband only)\n");
	printf("  -g, --dest_gid=<gid>  use gid as remote gid on destination port\n");
	printf("          <gid format>=xxxx:xxxx:xxxx:xxxx\n");
	printf("  -q, --dest_qpn=<qpn>  use qpn for remote queue pair number\n");
}

static int parse_gid(char *gid_str, union ibv_gid *gid) {
	uint16_t mcg_gid[8];
	char *ptr = gid_str;
	char *term = gid_str;
	int i = 0;

	term = strtok(gid_str, ":");
	while(1){ 
		mcg_gid[i] = htons((uint16_t)strtoll(term, NULL, 16));

		term = strtok(NULL, ":");
		if (term == NULL)
			break;

		if ((term - ptr) != 5) {
			fprintf(stderr, "Invalid GID format.\n");
			return -1;
		}
		ptr = term;

		i += 1;
	};

	if (i != 7) {
		fprintf(stderr, "Invalid GID format (2).\n");
		return -1;
	}

	memcpy(gid->raw, mcg_gid,16);
	return 0;
}



// usage: ./rdma_send <destnation qpn(十进制)>

// 实际过程中需要传入的参数：
// 1. devname
// 2. dest_gid_str
// 3. dev_port
// 4. dest_qpn(在recv端创建QP的时候生成)
int main(int argc, char** argv) {

    struct ibv_device **device_list;
    int    number_device;
    int    i;
    char   *devname = "mlx4_0";
    char   dest_gid_str[] = "fe80:0000:0000:0000:0202:c9ff:fe05:6a21"; // 2.1 10G网卡第一个端口
    // Infiniband only
    uint16_t  dest_lid = 0;
    int dev_port = 1; // 网卡的第一个端口为1

    // 用来计时
    clock_t start_alloc, finish_alloc;
    clock_t start_send, finish_send;

    //如何获得destination QP number?
    int dest_qpn = 0;

    dest_qpn = strtol(argv[1], NULL, 10);
    printf("dest_qpn: %d", dest_qpn);
    printf("argv[1]: %s\n", argv[1]);


    // 在最初准备好一个够大的内存池
    char *mr_buffer = (char*) malloc(MEM_POOL_SIZE * 1024 * 1024);
    if (!mr_buffer) {
    	fprintf(stderr, "Could't allocate %d MB memory.\n", MEM_POOL_SIZE);
    	return 0;
    }

    FILE *fp = NULL;

    fp = fopen("./test.jpg", "rb");
    if (!fp) {
    	fprintf(stderr, "Can't open target file.\n");
	return 0;
    }
    
    // 获得文件的大小
    long file_size = 0;
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    int read_bytes = 0;
    long total_read_bytes = 0;
    while ((read_bytes = fread(mr_buffer + sizeof(char)*total_read_bytes, 1, BLOCK_SIZE, fp)) != 0) {
	total_read_bytes += read_bytes;
    }
    
    #ifdef DEBUG
    printf("file_size = %ld bytes.\n", file_size);
    printf("read_file_size = %ld bytes.\n", total_read_bytes);
    #endif

    device_list = ibv_get_device_list(&number_device);

    // 没找到device_list的情况
    if (!device_list) {
        perror("Failed to get IB device list");
        return 1;
    }

    // 第一步：获取设备列表
    for (i = 0; i < number_device; i++) {
        if (!strcmp(ibv_get_device_name(device_list[i]), devname)) {
            break;
        }
        // printf("%s\n", ibv_get_device_name(device_list[i]));
    }

    if (i == number_device) {
        fprintf(stderr, "RDMA device %s not found.\n", devname);
        goto free_dev_list;
    }

    start_alloc = clock();

    struct ibv_device *device = device_list[i];

    // 第二步：打开设备(设备名指定)
    struct ibv_context *context = ibv_open_device(device);
    if (!context) {
        fprintf(stderr, "Could't get context for %s.\n", ibv_get_device_name(device));
        goto free_dev_list;
    }

    // 第三步：获得Protection Domain
    struct ibv_pd *pd = ibv_alloc_pd(context);
    if (!pd) {
        fprintf(stderr, "Couldn't allocate PD.\n");
        goto close_device;
    }

    // char mr_buffer[REGION_SIZE];


    // 第四步：将分配的这段MR注册给网卡
    // 查看了user manual手册，Permission这个参数设置为0代表只能本地读
    struct ibv_mr *mr = ibv_reg_mr(pd, mr_buffer, MEM_POOL_SIZE * 1024 * 1024, 0);
    if (!mr) {
        fprintf(stderr, "Couldn't register MR.\n");
        goto close_pd;
    }



    // printf("Register MR successful.\n");

    // 第五步：创建CQ
    // 后三个参数只有在处理completion events时使用
    // ？？回调函数是否可以在这里指定
    struct ibv_cq *cq = ibv_create_cq(context, CQ_SIZE, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "Couldn't create CQ.\n");
        goto free_mr;
    }

    // 第六步：创建QP
    // 设置QP属性
    struct ibv_qp_init_attr attr = {
        // qp_type: transport service type request for this QP
        // UD: unreliable datagram(这里之后需要修改！！)
        .qp_type = IBV_QPT_UD,
        .send_cq = cq,
        .recv_cq = cq,
        // QP send和QP recv元素的属性
        .cap = {
            //maximum number of outstanding WQ that can be posted to SQ
            .max_send_wr  = MAX_NUM_SENDS,
            .max_recv_wr  = 0,
            .max_send_sge = MAX_GATHER_ENTRIES,
            .max_recv_sge = MAX_SCATTER_ENTRIES,
        },
    };

    struct ibv_qp *qp = ibv_create_qp(pd, &attr);
    if (!qp) {
        fprintf(stderr, "Couldn't create QP.\n");
        goto free_cq;
    }

    // //********* For testing **********
    // struct ibv_device_attr dev_attr;
    // if (ibv_query_device(context, &dev_attr) != 0) {
    //     fprintf(stderr, "Errror in querying device infomation.");
    //     goto free_cq;
    // }
    // // 32 for this NIC
    // printf("Max scatter gatter elements supported by this NIC is: %d.\n", dev_attr.max_sge);

    // 第七步：将QP状态切换到RTS
    struct ibv_qp_attr qp_modify_attr;

    qp_modify_attr.qp_state = IBV_QPS_INIT; // translate to init state
    qp_modify_attr.pkey_index = 0;
    qp_modify_attr.port_num = dev_port; // primary physical port
    qp_modify_attr.qkey = WELL_KNOWN_QKEY; // For UD only

    // RESET -> INIT
    if (ibv_modify_qp(qp, &qp_modify_attr,
        IBV_QP_STATE        |
        IBV_QP_PKEY_INDEX   |
		IBV_QP_PORT         |
		IBV_QP_QKEY)) {
        fprintf(stderr, "Failed to modify QP to INIT.\n");
        goto free_qp;
    }
	
    memset(&qp_modify_attr, 0, sizeof(qp_modify_attr));

    qp_modify_attr.qp_state = IBV_QPS_RTR;
    
    // INIT -> RTR
    if (ibv_modify_qp(qp, &qp_modify_attr, IBV_QP_STATE)) {
        fprintf(stderr, "Failed to modify QP to RTR.\n");
        goto free_qp;
    }

    memset(&qp_modify_attr, 0, sizeof(qp_modify_attr));

    qp_modify_attr.qp_state = IBV_QPS_RTS;
    qp_modify_attr.sq_psn = 1;
    // RTR -> RTS
    if (ibv_modify_qp(qp, &qp_modify_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        fprintf(stderr, "Failed to modify QP to RTS.\n");
        goto free_qp;
    }
    
    // 第八步：创建Address Vector
    union ibv_gid dest_gid;
    if (parse_gid(dest_gid_str, &dest_gid)) {
        usage(argv[0]);
        goto free_qp;
    }
    
    struct ibv_ah_attr ah_attr;

    // indicating the presence of destination gid
    ah_attr.is_global = 1;
    ah_attr.grh.dgid = dest_gid;
    // the index of the source gid of our packet
    ah_attr.grh.sgid_index = 0;
    // achieved by running "ibstatus"
    ah_attr.dlid = dest_lid;
    // port num
    ah_attr.port_num = dev_port;
 	ah_attr.grh.hop_limit = 1;
	ah_attr.sl = 0;   

    struct ibv_ah *ah = ibv_create_ah(pd, &ah_attr);
    if (!ah) {
        fprintf(stderr, "Failed to address handle.\n");
        goto free_qp;
    }

    finish_alloc = clock();

    double alloc_time = (double)(finish_alloc - start_alloc) / CLOCKS_PER_SEC;

    start_send = clock();

    // 第九步：Post Work Request
    // work request data structure
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad_wr;
    struct ibv_sge list;


    // sprintf(mr_buffer, TEXT_MSG);
    list.addr = (uint64_t)mr_buffer;
    list.length = total_read_bytes;
    // 注册好了MR本身就带有了local key
    list.lkey = mr->lkey;

    // user defined 64-bit identifier, 
    // will be returned in the completion element for this WR
    wr.wr_id = 0;
    // a pointer to list of buffers(and the sizes where data is located)
    wr.sg_list = &list;
    // number of entries in scatter gatter lists
    wr.num_sge = 1;
    // operation we will perform
    wr.opcode = IBV_WR_SEND;
    // set individual properties:
    // IBV_SEND_SIGNALED means generation of a completion element
    // once the data is transmitted
    wr.send_flags = IBV_SEND_SIGNALED;
    // next指向了下一个WR（在post了一系列WR时候用到）
    // 因为我们只是post了一个Send,所以这里使用NULL
    wr.next = NULL;

    // specify the target destination using previous created address vector
    wr.wr.ud.ah = ah;
    // specify the target QP number
    wr.wr.ud.remote_qpn = dest_qpn;
    // qkey value of remote QP
    // WELL_KNOWN_QKEY generatee all QP in this example share the same qkey
    wr.wr.ud.remote_qkey = WELL_KNOWN_QKEY;

    // 三个参数：
    // 1. created QP
    // 2. pointer to the first WR 
    // 3. pointer to a list of Bad Work Request in case of failure
    if (ibv_post_send(qp, &wr, &bad_wr)) {
        fprintf(stderr, "Function ibv_post_send failed.\n");
        goto free_qp;
    }

    // 第十步：Poll for completion (retrieve Work Completion from CQ)
    
    struct ibv_wc wc;
    int ne;

    // Arguments:
    // 1. CQ be retrieved
    // 2. maximum number of completions to read from a CQ
    // 3. points to a array of CQ entries to be filled in case completions to be found
    // Return:
    // if one or more entries are found, this call returns the number of completed entries(但不会超过num_entries,即第二个参数)
    // if no entries to be found, this call will return 0
    do {
        ne = ibv_poll_cq(cq, 1, &wc);
    } while (ne == 0);

    // call return negative value if function fails
    if (ne < 0) {
        fprintf(stderr, "CQ is in error state.");
        goto free_qp;
    }

    if (wc.status) {
        fprintf(stderr, "Bad completion (status %d).\n", (int)wc.status);
        goto free_qp;
    }

    finish_send = clock();
    double send_time = (double)(finish_send - start_send) / CLOCKS_PER_SEC;

    printf("Allocation process spend %.5f seconds.\n", alloc_time);
    printf("Sending process spend %.5f seconds.\n", send_time);

free_qp:    
    ibv_destroy_qp(qp);

free_cq:
    ibv_destroy_cq(cq);

free_mr:
    ibv_dereg_mr(mr);

close_pd:
    ibv_dealloc_pd(pd);

close_device:
    ibv_close_device(context);

free_dev_list:
    ibv_free_device_list(device_list);

    return 0;
}
