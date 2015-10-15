#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>

#include <getopt.h>

#define BUFFER_SIZE 80
#define MAX_PROBES 16
#define AVERAGER_SIZE 400

int average_n = 10;//0.025s * 400 Hz

typedef struct average_data {
        int data_count;
        int circular_index;
        int buffer[AVERAGER_SIZE];
        double average;
        int median;
        double rmsd;
} average_data_t;

void init_average(average_data_t *avg){
        memset(avg, 0, sizeof(average_data_t));
}

int cmp_int(const void *p1, const void *p2){
        return (*(int*)p1)-(*(int*)p2);
}

void update_average_from_array(average_data_t *avg, int* array, int size){
        double average = 0;
        for(int i=0;i<size;i++){
                average+=array[i];
        }
        avg->average = average/size;

        double rmsd = 0;
        for(int i=0;i<size;i++){
                double tmp = array[i]-avg->average;
                rmsd+=tmp*tmp;
        }
        avg->rmsd = sqrt(rmsd/size);

}

void update_average(average_data_t *avg, int value){

        avg->buffer[avg->circular_index]=value;
        avg->data_count++;
        avg->circular_index++;

        if(avg->data_count>=average_n){
                int median[AVERAGER_SIZE];
                int median_idx = 0;
                if(avg->circular_index>=average_n){
                        for(int i=avg->circular_index-average_n;i<avg->circular_index;i++){
                                median[median_idx++]=avg->buffer[i];
                        }
                }else{
                        for(int i=AVERAGER_SIZE-(average_n-avg->circular_index);i<AVERAGER_SIZE;i++){
                                median[median_idx++]=avg->buffer[i];
                        }
                        for(int i=0;i<avg->circular_index;i++){
                                median[median_idx++]=avg->buffer[i];
                        }
               }
               assert(median_idx==average_n);
               update_average_from_array(avg, median, average_n);

               qsort(median, average_n, sizeof(int), cmp_int);
               avg->median = median[average_n/2];
        }

        if(avg->circular_index>=AVERAGER_SIZE){
                avg->circular_index = 0;
        }
}

typedef struct probe_data{
        int time;
        int last_value;

        int rising_edge_time;
        int falling_edge_time;

        average_data_t pulse_width_avg;
        average_data_t period_avg;

} probe_data_t;

typedef struct context{
        int probes_n;
        probe_data_t probes[MAX_PROBES];
        int max_time;
} context_t;


int from_hex(char digit){
        if(digit>='0' && digit<='9'){
                return digit-'0';
        }
        if(digit>='a' && digit<='f'){
                return digit-'a'+10;
        }
        if(digit>='A' && digit<='F'){
                return digit-'A'+10;
        }
        return -1;
}

int feed_data(probe_data_t *data, int digit){
        if(digit<0 || digit>15){
                return 3;
        }
        for(int i=0;i<3;i++){
                int value = digit>>3;
                if(data->last_value!=0 && value==0){
                        //falling edge
                        if(data->rising_edge_time>=0){
                                update_average(&data->pulse_width_avg, data->time - data->rising_edge_time);
                        }
                        data->falling_edge_time = data->time;
                }
                if(data->last_value==0 && value!=0){
                        //rising edge
                        if(data->rising_edge_time>=0){
                                update_average(&data->period_avg, data->time - data->rising_edge_time);
                        }
                        data->rising_edge_time = data->time;
                }

                data->time++;
                digit<<=1;
        }
        return 0;
}

int process_probe(context_t *ctx, int probe_idx, char buffer[]){
        if(probe_idx<0 || probe_idx>=MAX_PROBES){
                return 1;
        }
        if(probe_idx>=ctx->probes_n){
                int i = ctx->probes_n;
                while(i<=probe_idx){
                        //TODO: different start time for probes?
                        probe_data_t *probe = &ctx->probes[i];
                        probe->time = 0;
                        probe->last_value = 0;
                        probe->rising_edge_time = -1;
                        probe->falling_edge_time = -1;
                        init_average(&probe->period_avg);
                        init_average(&probe->pulse_width_avg);
                }
                ctx->probes_n = probe_idx+1;
        }
        int p=2;
        probe_data_t *probe = &ctx->probes[probe_idx];
        int time = probe->time;
        while(p<BUFFER_SIZE-3 && buffer[p]!='\n' && buffer[p]!=0){
                int digit_h = from_hex(buffer[p]);
                int digit_l = from_hex(buffer[p+1]);
                char space = buffer[p+2];
                if(space!=' ' || digit_h<0 || digit_l<0){
                        return 2;
                }
                int r;
                r = feed_data(probe, digit_h);
                if(r){
                        fprintf(stderr, "feed_data digit_h error %d", r);
                        return r;
                }
                feed_data(probe, digit_l);
                if(r){
                        fprintf(stderr, "feed_data digit_l error %d", r);
                        return r;
                }
                time = probe->time;
                p+=3;
        }
        if(time>ctx->max_time){
                ctx->max_time = time;//where is my max?
        }
        return 0;
}

void show_help(){
        printf("Usage: pwm [-n buffer_length] [-h] < sigrok_hex_file\n");
}

int main(int argc, char** argv){
        char buffer[BUFFER_SIZE];
        bool started=false;
        context_t context;
        int line_num=1;
        context.probes_n = 0;
        context.max_time = 0;
        int ch;

        static struct option longopts[] = {
                { "help", no_argument, NULL, 'h' },
                { "length", required_argument, NULL, 'n' },
                { NULL, 0, NULL, 0 }
        };

        while ((ch = getopt_long(argc, argv, "hn:", longopts, NULL)) != -1)
                switch (ch) {
                case 'h':
                    show_help(); 
                    return 0;
                case 'n':
                    average_n = atoi(optarg);
                    break;
                default:
                    show_help();
                    return 1;
        }


        while(fgets(buffer, BUFFER_SIZE, stdin)){
                //printf("%p %s\n", buffer, buffer);
                char probe = buffer[0];
                bool is_data = (probe>='0' && probe<='9' && buffer[1]==':');

                started |= is_data;
                if(started && !is_data){
                        fprintf(stderr, "error, no data line %d %s",line_num, buffer);
                        return 1;
                }
                if(started){
                        int r = process_probe(&context, probe-'0', buffer);
                        if(r){
                                fprintf(stderr, "process_probe error %d line:%d %s", r, line_num, buffer);
                                return r;
                        }
                }
                line_num++;
        }
        return 0;
}

