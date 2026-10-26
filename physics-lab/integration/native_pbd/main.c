// LIMINAL native NVIDIA PhysX 5.12 PBD via official ovphysx 0.7 C API.
// This is an isolated physics lab, NOT the IX-Ray game runtime.
#include "ovphysx/ovphysx.h"
#include "ovphysx/ovphysx_attributes.h"
#include "ovstage_sample.h"
#include <stdio.h>
#include <stdint.h>

static int finish(ovphysx_instance_handle_t p, ovphysx_enqueue_result_t q) {
    if (q.status != OVPHYSX_API_SUCCESS) return 0;
    ovphysx_wait_task_result_t waited={0};
    ovphysx_result_t done=ovphysx_wait_task(p,q,OVPHYSX_TIMEOUT_INFINITE,&waited);
    int good= (done.status==OVPHYSX_API_SUCCESS && waited.num_errors==0);
    if (!good) fprintf(stderr,"OVPHYSX_TASK_FAIL errors=%zu\n",waited.num_errors);
    ovphysx_destroy_wait_task_result(&waited);
    return good;
}
static int verify_real_pbd(ovphysx_instance_handle_t p) {
    ovphysx_query_handle_t query=0;
    ovphysx_read_handle_t rd=0;
    int opened=0, particles=0, ok=1;
    const ovx_string_or_token_t point_attr={0,{OVPHYSX_ATTR_POINTS,sizeof(OVPHYSX_ATTR_POINTS)-1}};
    if (!finish(p, ovphysx_query(p,OVPHYSX_QUERY_TARGET_PARTICLE_SET,OVPHYSX_SCOPE_ALL,&query,NULL)))
        return 0;
    if (!finish(p, ovphysx_read(p,query,&point_attr,1,&rd,NULL))) {ok=0; goto release_q;}
    opened=1;
    for (;;) {
        const ovstage_read_group_t *g=NULL;
        if (!finish(p,ovphysx_fetch_read_next(p,rd,&g))) {ok=0;break;}
        if (!g) break;
        for (size_t k=0;k<g->data.tensor_count;k++) {
            const DLTensor *tensor=&g->data.tensors[k];
            if (tensor->ndim<1) continue;
            particles+=(int)tensor->shape[0];
            printf("NATIVE_PBD_PARTICLES %lld device=%d\n",
              (long long)tensor->shape[0],(int)tensor->device.device_type);
        }
        if (!finish(p,ovphysx_release_group(p,rd,g->read_group_id))) {ok=0;break;}
    }
    if (opened && !finish(p,ovphysx_release_read(p,rd))) ok=0;
release_q:
    if (!finish(p,ovphysx_release_query(p,query))) ok=0;
    return ok && particles==420;
}
int main(int argc,char **argv) {
    if(argc<2){fputs("Usage: liminal_native_pbd_shot.exe scene.usda\n",stderr);return 2;}
    if(ovphysx_initialize().status!=OVPHYSX_API_SUCCESS)return 3;
    int code=0;
    ovphysx_instance_handle_t instance=0;
    ovphysx_sample_stage_attachment_t stage={0};
    if(ovphysx_create_instance(&instance).status!=OVPHYSX_API_SUCCESS){code=4;goto end;}
    if(!ovphysx_sample_attach_usd_with_ovstage(instance,argv[1],&stage)){code=5;goto destroy;}
    printf("PBD_SCENE_ATTACHED %s\n",argv[1]);
    if(!verify_real_pbd(instance)){fputs("GPU_PBD_PARTICLE_GROUP_NOT_FOUND\n",stderr);code=8;goto detach;}
    for(int i=1;i<=132;i++) {
        if(!finish(instance,ovphysx_step(instance,1.0f/120.0f))){code=7;break;}
        if(i%24==0)printf("STEP %d/132\n",i);
    }
    if(!code){
        if(!verify_real_pbd(instance))code=9;
        else puts("NATIVE_PBD_PARTICLE_GPU_SIM_PASS");
    }
detach:
    ovphysx_sample_destroy_stage(instance,&stage);
destroy:
    ovphysx_destroy_instance(instance);
end:
    ovphysx_shutdown();
    return code;
}
