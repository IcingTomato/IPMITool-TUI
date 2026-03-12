// main.c - IPMI TUI (btop++风格) for Linux, pure C + ncurses
#include <ncurses.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <locale.h>
#include <ctype.h>

#define CONFIG_PATH "./config.ini"
#define MAX_SENSORS 16
#define HISTORY_LEN 40

struct Sensor {
    char name[64];
    float history[HISTORY_LEN];
    int hist_idx;
    int count;   // 有效读数数量，首次读数预填充 history 用
};

struct AppConfig {
    char mode[16];
    char host[64];
    char username[32];
    char password[32];
    int refresh_interval;
    int remember_cred;
};

struct AppState {
    struct AppConfig config;
    struct Sensor temps[MAX_SENSORS];
    int num_temps;
    struct Sensor fans[MAX_SENSORS];
    int num_fans;
    char power[32];
    char bmc[32];
    char sel[5][128];
    char last_error[128];
    int fetching;  // 1=正在后台拉取数据
};

void draw_box(int y, int x, int h, int w, int id, const char *title) {
    // 移除阴影，使用扁平暗色边框更接近 btop 风格
    attron(COLOR_PAIR(6)); 
    mvprintw(y, x, "╭");
    for(int i=1; i<w-1; ++i) mvprintw(y, x+i, "─");
    mvprintw(y, x+w-1, "╮");
    for(int i=1; i<h-1; ++i) {
        mvprintw(y+i, x, "│");
        mvprintw(y+i, x+w-1, "│");
    }
    mvprintw(y+h-1, x, "╰");
    for(int i=1; i<w-1; ++i) mvprintw(y+h-1, x+i, "─");
    mvprintw(y+h-1, x+w-1, "╯");
    attroff(COLOR_PAIR(6));

    if(title && strlen(title)>0) {
        if (id > 0) {
            // btop 样式的标题栏，左侧带序号 ╭─1info──
            attron(COLOR_PAIR(6)); mvprintw(y, x+1, "─"); attroff(COLOR_PAIR(6));
            attron(COLOR_PAIR(4)); mvprintw(y, x+2, "%d", id); attroff(COLOR_PAIR(4));
            attron(A_BOLD); mvprintw(y, x+3, "%s", title); attroff(A_BOLD);
            attron(COLOR_PAIR(6)); mvprintw(y, x+3+strlen(title), "─"); attroff(COLOR_PAIR(6));
        } else {
            // 没有序号时的标题栏 ╭─config──
            attron(COLOR_PAIR(6)); mvprintw(y, x+1, "─"); attroff(COLOR_PAIR(6));
            attron(A_BOLD); mvprintw(y, x+2, "%s", title); attroff(A_BOLD);
            attron(COLOR_PAIR(6)); mvprintw(y, x+2+strlen(title), "─"); attroff(COLOR_PAIR(6));
        }
    }
}

void draw_separator(int y, int x, int w, const char *title) {
    attron(COLOR_PAIR(6));
    mvprintw(y, x, "├");
    for(int i=1; i<w-1; ++i) mvprintw(y, x+i, "─");
    mvprintw(y, x+w-1, "┤");
    if(title && strlen(title)>0) {
        attron(A_BOLD);
        mvprintw(y, x+2, "%s", title);
        attroff(A_BOLD);
        mvprintw(y, x+2+strlen(title), "─");
    }
    attroff(COLOR_PAIR(6));
}

void draw_sensor_chart(int y, int x, int w, struct Sensor *s) {
    int valid_count = (s->count < HISTORY_LEN) ? s->count : HISTORY_LEN;
    float mx = 0, mn = 10000;
    if (valid_count == 0) {
        mx = 1.0; mn = 0.0;
    } else {
        for(int i=0; i<valid_count; ++i) {
            int idx = (s->hist_idx - valid_count + i + HISTORY_LEN) % HISTORY_LEN;
            if(s->history[idx] > mx) mx = s->history[idx];
            if(s->history[idx] < mn) mn = s->history[idx];
        }
    }
    // 计算动态范围，给予一定的上下文余量，使微小波动也能呈现，但避免因为全是同一个值导致渲染死板
    if(mx - mn < 2.0) {
        mx += 1.0;
        mn -= 1.0;
    }
    
    int flash = (time(NULL)%2==0); // 闪烁动画
    // 将原本的粗方块改为精细的盲文点阵 (Braille patterns)，类似于 btop++
    const char* blocks[] = {" ", "⡀", "⣀", "⣄", "⣤", "⣦", "⣶", "⣷", "⣿"};
    int num_blocks = 9;
    
    for(int i=0;i<w;++i) {
        // 绘制宽度超过历史记录时，左侧留白
        if (i < w - HISTORY_LEN) {
            mvprintw(y, x+i, " ");
            continue;
        }

        // 取最新的记录：宽度小于时截取最新，宽度大于时靠右对齐
        int data_offset = (w < HISTORY_LEN) ? (HISTORY_LEN - w + i) : (i - (w - HISTORY_LEN));
        
        // 如果数据还没收集到这个偏移量，就不绘制（留白），实现从右向左徐徐推进的效果
        if (data_offset < HISTORY_LEN - valid_count) {
            mvprintw(y, x+i, " ");
            continue;
        }

        int idx = (s->hist_idx + data_offset) % HISTORY_LEN;
        float v = s->history[idx];

        int level = (int)((v-mn)/(mx-mn)*(num_blocks-0.01)); // 强映射到 0 到 num_blocks-1 
        if(level < 0) level = 0;
        if(level >= num_blocks) level = num_blocks - 1;
        
        // 动态基于绝对数值决定颜色（例如，温度/风扇报警阈值可调，这处假设：低绿、中黄、高红）
        // 对于风扇（数值通常大于500），定义：<2000 绿，2000-5000 黄， >5000 红
        // 对于温度（数值通常小于200），定义：<40 绿，40-65 黄， >65 红
        int color_pair = 3; // 默认绿
        int is_temp = 0;
        if (v > 500) { // 大概率是风扇
            if (v > 8000) color_pair = 4; // 红
            else if (v > 4000) color_pair = 2; // 黄
            else color_pair = 3; // 绿
        } else { // 大概率是温度
            is_temp = 1;
            if (v > 70) color_pair = 4; // 红
            else if (v > 45) color_pair = 2; // 黄
            else color_pair = 3; // 绿
        }
        
        attron(COLOR_PAIR(color_pair));
        
        // 仅温度极高时闪烁，风扇即使红警也不闪烁
        if(color_pair == 4 && is_temp && v > 75 && flash) attron(A_BLINK);
        mvprintw(y, x+i, "%s", blocks[level]);
        attroff(A_BLINK);
        attroff(COLOR_PAIR(color_pair));
    }
    // 动画符号与当前值
    static const char *anim[] = {"|", "/", "-", "\\"};
    int frame = (time(NULL)%4);
    int last_idx = (s->hist_idx - 1 + HISTORY_LEN) % HISTORY_LEN;
    attron(A_BOLD);
    // 使用 %7.1f 适应上万的风扇转速，确保对齐且不覆盖其他内容
    mvprintw(y, x + w + 1, "%7.1f %s", s->history[last_idx], anim[frame]);
    attroff(A_BOLD);
}

void load_config(struct AppConfig *cfg) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if(!f) { // defaults
        strcpy(cfg->mode, "inband");
        cfg->host[0]=0; cfg->username[0]=0; cfg->password[0]=0;
        cfg->refresh_interval=3; cfg->remember_cred=0;
        return;
    }
    char line[128];
    while(fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if(!eq) continue;
        *eq=0; char *key=line, *val=eq+1;
        val[strcspn(val,"\r\n")]=0;
        if(strcmp(key,"mode")==0) strncpy(cfg->mode,val,sizeof(cfg->mode)-1);
        else if(strcmp(key,"host")==0) strncpy(cfg->host,val,sizeof(cfg->host)-1);
        else if(strcmp(key,"username")==0) strncpy(cfg->username,val,sizeof(cfg->username)-1);
        else if(strcmp(key,"password")==0) strncpy(cfg->password,val,sizeof(cfg->password)-1);
        else if(strcmp(key,"refresh_interval")==0) cfg->refresh_interval=atoi(val);
        else if(strcmp(key,"remember_cred")==0) cfg->remember_cred=atoi(val);
    }
    fclose(f);
}

void save_config(struct AppConfig *cfg) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if(!f) return;
    fprintf(f, "mode=%s\nhost=%s\nusername=%s\npassword=%s\nrefresh_interval=%d\nremember_cred=%d\n",
        cfg->mode, cfg->host, cfg->username, cfg->password, cfg->refresh_interval, cfg->remember_cred);
    fclose(f);
}

/* 根据配置构建 ipmitool 命令前缀，OOB 模式加上 -I lanplus 参数，
   并将 stderr 重定向到 /dev/null 防止错误信息污染 TUI 界面 */
static void build_cmd(char *buf, size_t bufsz,
                      const struct AppConfig *cfg, const char *subcmd) {
    if(strcmp(cfg->mode, "oob") == 0) {
        /* -N 4: 单次响应超时4s; -R 1: 不重试; 外层 timeout 10 兜底防挂死 */
        snprintf(buf, bufsz,
            "timeout 10 ipmitool -I lanplus -N 4 -R 1 -H '%s' -U '%s' -P '%s' %s 2>/dev/null",
            cfg->host, cfg->username, cfg->password, subcmd);
    } else {
        snprintf(buf, bufsz, "timeout 5 ipmitool %s 2>/dev/null", subcmd);
    }
}

void fetch_ipmi(struct AppState *state) {
    state->num_temps=0; state->num_fans=0;
    state->power[0]=0; state->bmc[0]=0; state->last_error[0]=0;

    char cmd[512];

    /* OOB 模式下若 host 为空，直接报错跳过 */
    if(strcmp(state->config.mode, "oob") == 0 && state->config.host[0] == '\0') {
        snprintf(state->last_error, sizeof(state->last_error),
                 "OOB mode: host not configured");
        return;
    }

    // chassis power
    build_cmd(cmd, sizeof(cmd), &state->config, "chassis power status");
    FILE *fp = popen(cmd, "r");
    if(fp) {
        if(!fgets(state->power, sizeof(state->power), fp))
            state->power[0] = '\0';
        state->power[strcspn(state->power,"\r\n")] = 0;
        int rc = pclose(fp);
        if(rc != 0 && state->power[0] == '\0')
            snprintf(state->last_error, sizeof(state->last_error),
                     "ipmitool error (power), rc=%d", rc);
    }

    // mc info
    build_cmd(cmd, sizeof(cmd), &state->config, "mc info");
    fp = popen(cmd, "r");
    if(fp) {
        char line[128];
        while(fgets(line,sizeof(line),fp)) {
            if(strstr(line,"Firmware Revision")) {
                char *p = strchr(line, ':');
                if(p) { strcpy(state->bmc, p+2); }
                state->bmc[strcspn(state->bmc,"\r\n")] = 0;
            }
        }
        pclose(fp);
    }

    // sensor
    build_cmd(cmd, sizeof(cmd), &state->config, "sensor");
    fp = popen(cmd, "r");
    if(fp) {
        char line[256];
        while(fgets(line,sizeof(line),fp)) {
            if(!strstr(line,"|")) continue;
            /* 复制一份用于 strtok，保留原行做关键字匹配 */
            char linecopy[256];
            strncpy(linecopy, line, sizeof(linecopy)-1);
            linecopy[sizeof(linecopy)-1] = '\0';
            char *name = strtok(linecopy, "|");
            char *val  = strtok(NULL, "|");
            char *unit = strtok(NULL, "|");
            if(!name || !val || !unit) continue;
            /* 去除首部空格 */
            while(*name==' ' || *name=='\t') name++;
            while(*val ==' ' || *val =='\t') val++;
            while(*unit==' ' || *unit=='\t') unit++;
            /* 去除尾部空格（保留传感器全名，如 "Inlet Temp"） */
            int nlen=(int)strlen(name); while(nlen>0 && (name[nlen-1]==' '||name[nlen-1]=='\t')) name[--nlen]='\0';
            int vlen=(int)strlen(val);  while(vlen>0 && (val [vlen-1]==' '||val [vlen-1]=='\t')) val [--vlen]='\0';
            int ulen=(int)strlen(unit); while(ulen>0 && (unit[ulen-1]==' '||unit[ulen-1]=='\t')) unit[--ulen]='\0';

            float v = atof(val);
            /* 过滤无效值（ipmitool 用 "na" 表示无效传感器） */
            if(val[0]=='n' || val[0]=='N') continue;

            /* 温度传感器 */
            char line_lower[256];
            strncpy(line_lower, line, sizeof(line_lower)-1);
            for(int k=0; line_lower[k]; k++) line_lower[k]=(char)tolower((unsigned char)line_lower[k]);

            if((strstr(line_lower,"temp") || strstr(name,"Temp"))
               && state->num_temps < MAX_SENSORS) {
                int idx = state->num_temps;
                strncpy(state->temps[idx].name, name, 63);
                state->temps[idx].name[63] = '\0';
                state->temps[idx].history[state->temps[idx].hist_idx] = v;
                state->temps[idx].hist_idx = (state->temps[idx].hist_idx + 1) % HISTORY_LEN;
                state->temps[idx].count++;
                state->num_temps++;
            }
            /* 风扇传感器 */
            if((strstr(line_lower,"fan") || strstr(name,"Fan"))
               && state->num_fans < MAX_SENSORS) {
                int idx = state->num_fans;
                strncpy(state->fans[idx].name, name, 63);
                state->fans[idx].name[63] = '\0';
                state->fans[idx].history[state->fans[idx].hist_idx] = v;
                state->fans[idx].hist_idx = (state->fans[idx].hist_idx + 1) % HISTORY_LEN;
                state->fans[idx].count++;
                state->num_fans++;
            }
        }
        pclose(fp);
    }

    // sel list (最新 5 条)
    build_cmd(cmd, sizeof(cmd), &state->config, "sel list last 5");
    fp = popen(cmd, "r");
    if(fp) {
        int i = 0; char line[128];
        while(fgets(line,sizeof(line),fp) && i<5) {
            strncpy(state->sel[i], line, 127);
            state->sel[i][strcspn(state->sel[i],"\r\n")] = 0;
            i++;
        }
        pclose(fp);
    }
}

void draw_config_editor(struct AppConfig *cfg) {
    clear();
    draw_box(1,2,12,60,0,"config");
    mvprintw(3,4,"Mode (inband/oob): %s",cfg->mode);
    mvprintw(4,4,"Host: %s",cfg->host);
    mvprintw(5,4,"Username: %s",cfg->username);
    mvprintw(6,4,"Password: %s",cfg->password);
    mvprintw(7,4,"Refresh Interval: %d",cfg->refresh_interval);
    mvprintw(8,4,"Remember Creds: %d",cfg->remember_cred);
    mvprintw(10,4,"[E]dit field  [S]ave  [Q]uit config");
    refresh();
}

void edit_config(struct AppConfig *cfg) {
    int ch;
    timeout(-1); // 进入编辑界面时切为完全阻塞，防止 500ms 超时清屏
    while(1) {
        draw_config_editor(cfg);
        ch = getch();
        if(ch=='q'||ch=='Q') break;
        if(ch=='s'||ch=='S') { save_config(cfg); break; }
        if(ch=='e'||ch=='E') {
            mvprintw(12,4,"Field (1-mode 2-host 3-user 4-pass 5-interval 6-remember): ");
            refresh();
            int f = getch()-'0';
            char buf[64];
            echo(); curs_set(1);
            switch(f) {
                case 1: mvprintw(13,4,"New mode: "); getnstr(buf,15); strcpy(cfg->mode,buf); break;
                case 2: mvprintw(13,4,"New host: "); getnstr(buf,63); strcpy(cfg->host,buf); break;
                case 3: mvprintw(13,4,"New username: "); getnstr(buf,31); strcpy(cfg->username,buf); break;
                case 4: mvprintw(13,4,"New password: "); getnstr(buf,31); strcpy(cfg->password,buf); break;
                case 5: mvprintw(13,4,"New interval: "); getnstr(buf,7); cfg->refresh_interval=atoi(buf); break;
                case 6: mvprintw(13,4,"Remember creds (0/1): "); getnstr(buf,3); cfg->remember_cred=atoi(buf); break;
            }
            noecho(); curs_set(0);
        }
    }
    timeout(500); // 退出编辑界面后恢复非阻塞模式
}

void draw_main(struct AppState *state) {
    // 移除 clear()，防止全屏闪烁。我们依靠后面的全屏空格或具体内容覆盖
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    // 清理一下上一次可能残留的画面剩余部分（背景填充），但不引发整屏同时清空的重绘闪烁
    for(int i=0; i<max_y; i++) {
        mvhline(i, 0, ' ', max_x);
    }
    
    int sel_h = 7; // 底部 SEL 面板高度
    int top_h = max_y - 2 - sel_h; // 上方内容区总高度 (-2是状态栏)
    if(top_h < 12) top_h = 12; // 保证上方有足够空间
    
    int left_w = 32;
    int right_w = max_x - left_w - 1;
    if(right_w < 30) {
        right_w = 30; // 最小宽度
        left_w = max_x - right_w - 1;
        if(left_w < 20) left_w = 20;
    }
    
    int temp_h = top_h / 2;
    int fan_h = top_h - temp_h;
    
    draw_box(0,0,top_h,left_w,0,"config");
    
    attron(A_BOLD | COLOR_PAIR(1));
    mvprintw(2,2,"Mode: %s",state->config.mode);
    mvprintw(3,2,"Host: %s",state->config.host);
    mvprintw(4,2,"User: %s",state->config.username);
    attroff(A_BOLD | COLOR_PAIR(1));
    
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(5,2,"Refresh: %d s",state->config.refresh_interval);
    mvprintw(6,2,"Remember: %s",state->config.remember_cred ? "yes" : "no");
    attroff(A_BOLD | COLOR_PAIR(3));

    draw_separator(8, 0, left_w, "info");
    
    attron(A_BOLD | COLOR_PAIR(2));
    mvprintw(9,2,"Power:");
    mvprintw(10,2,"BMC FW:");
    attroff(A_BOLD | COLOR_PAIR(2));
    mvprintw(9,9,"%s", state->power);
    mvprintw(10,10,"%s", state->bmc);

    // 绘制下方的长条 SEL
    int sel_start_y = top_h;
    draw_box(sel_start_y, 0, sel_h, max_x, 0, "sel");
    
    /* SEL: 解析 ipmitool 格式 "ID | date | time | sensor | event | dir" */
    for(int i=0;i<5 && i < sel_h-2; ++i) {
        if(state->sel[i][0] == '\0') continue;
        char selcopy[128];
        strncpy(selcopy, state->sel[i], 127); selcopy[127]='\0';
        
        char *tok = strtok(selcopy, "|"); // ID
        char *date = strtok(NULL, "|");   // date
        char *ttime = strtok(NULL, "|");  // time
        char *sensor_f = strtok(NULL, "|"); // sensor
        char *event_f  = strtok(NULL, "|"); // event
        
        if(date && ttime && event_f) {
            /* 去首尾空格 */
            while(*date==' ') date++; char *e; if((e=strrchr(date,' '))) *e='\0';
            while(*ttime==' ') ttime++; if((e=strrchr(ttime,' '))) *e='\0';
            if(!sensor_f) sensor_f = "";
            else { while(*sensor_f==' ') sensor_f++; if((e=strrchr(sensor_f,' '))) *e='\0'; }
            while(*event_f==' ') event_f++; if((e=strrchr(event_f,' '))) *e='\0';
            
            attron(COLOR_PAIR(3) | A_BOLD);
            mvprintw(sel_start_y + 1 + i, 2, "%-10.10s", date); // Date
            mvprintw(sel_start_y + 1 + i, 13, "%-8.8s", ttime); // Time
            attroff(COLOR_PAIR(3) | A_BOLD);
            
            attron(COLOR_PAIR(1));
            mvprintw(sel_start_y + 1 + i, 23, "%-20.20s", sensor_f); // Sensor
            attroff(COLOR_PAIR(1));
            
            attron(COLOR_PAIR(4));
            mvprintw(sel_start_y + 1 + i, 45, "%.*s", max_x - 47, event_f); // Event
            attroff(COLOR_PAIR(4));
        } else {
            /* fallback: 显示完整原文 */
            attron(COLOR_PAIR(4));
            mvprintw(sel_start_y + 1 + i, 2, "%.*s", max_x - 4, state->sel[i]);
            attroff(COLOR_PAIR(4));
        }
    }

    draw_box(0, left_w, temp_h, right_w, 0, "temps");
    for(int i=0;i<state->num_temps && 2+i < temp_h-1;++i) {
        attron(COLOR_PAIR(2) | A_BOLD);
        mvprintw(2+i, left_w+2, "%-8.8s",state->temps[i].name);
        attroff(COLOR_PAIR(2) | A_BOLD);
        // 让出多些空间给后面的数值 (之前是17，现在让出21列空间)
        int chart_w = right_w - 21;
        if(chart_w > 0) draw_sensor_chart(2+i, left_w+11, chart_w, &state->temps[i]);
    }
    
    draw_box(temp_h, left_w, fan_h, right_w, 0, "fans");
    for(int i=0;i<state->num_fans && temp_h+1+i < top_h-1;++i) { // 修正这里，限制不越过 top_h
        attron(COLOR_PAIR(1) | A_BOLD);
        mvprintw(temp_h+1+i, left_w+2, "%-8.8s",state->fans[i].name);
        attroff(COLOR_PAIR(1) | A_BOLD);
        // 让出多些空间给后面的转速 (同上)
        int chart_w = right_w - 21;
        if(chart_w > 0) draw_sensor_chart(temp_h+1+i, left_w+11, chart_w, &state->fans[i]);
    }
    
    // 状态栏 (btop 风格)
    for(int i=0; i<max_x; i++) mvprintw(max_y-1, i, " ");
    
    attron(COLOR_PAIR(6) | A_BOLD); 
    mvprintw(max_y-1, 0, " select ");
    attroff(COLOR_PAIR(6) | A_BOLD);
    
    attron(COLOR_PAIR(4)); mvprintw(max_y-1, 8, "C"); attroff(COLOR_PAIR(4));
    printw("onfig   ");
    attron(COLOR_PAIR(4)); printw("Q"); attroff(COLOR_PAIR(4));
    printw("uit ");

    if(state->fetching) {
        attron(COLOR_PAIR(3) | A_BOLD);
        printw("  ⟳ fetching... ");
        attroff(COLOR_PAIR(3) | A_BOLD);
    } else if(strlen(state->last_error) > 0) {
        attron(COLOR_PAIR(4) | A_BOLD);
        printw("  Error: %s ",state->last_error);
        attroff(COLOR_PAIR(4) | A_BOLD);
    }

    attron(COLOR_PAIR(6) | A_DIM);
    mvprintw(max_y-1, max_x-22, "mode:%s rfrsh:%ds", state->config.mode, state->config.refresh_interval);
    attroff(COLOR_PAIR(6) | A_DIM);
    
    refresh();
}

int main() {
    setlocale(LC_ALL, "");
    
    // 发送 ANSI 转义序列请求终端模拟器将窗口强制调整为 40 行 95 列
    printf("\033[8;40;95t");
    fflush(stdout);
    usleep(50000); // 稍微等待 50 毫秒，让终端模拟器有时间处理缩放
    
    struct AppState state;
    memset(&state, 0, sizeof(state)); // 关键：全部初始化为零，防止 hist_idx 随机值越界崩溃
    load_config(&state.config);
    initscr(); cbreak(); noecho(); keypad(stdscr,TRUE);
    curs_set(0);  // 主界面隐藏光标
    timeout(500); // 500ms 不阻塞，用于处理调整大小和定时刷新
    signal(SIGINT,SIG_IGN);
    if(has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);      // old var
        init_pair(2, COLOR_YELLOW, -1);    // sensor name
        init_pair(3, COLOR_GREEN, -1);     // normal value
        init_pair(4, COLOR_RED, -1);       // high temp/fan & warning
        init_pair(5, COLOR_WHITE, COLOR_BLUE); 
        init_pair(6, COLOR_WHITE, -1);     // standard border color
        // 尝试使用高亮颜色 (Bright colors)
        init_pair(7, COLOR_MAGENTA, -1);
    }
    // 启动立即画一帧，避免黑屏等待
    draw_main(&state);

    int ch;
    time_t last_refresh = 0;
    while(1) {
        time_t now = time(NULL);
        if(now - last_refresh >= state.config.refresh_interval) {
            // 先刷新屏幕显示 Fetching 指示，再阻塞拉取数据
            state.fetching = 1;
            draw_main(&state);
            fetch_ipmi(&state);
            state.fetching = 0;
            last_refresh = time(NULL);
        }
        draw_main(&state);
        ch = getch();
        if(ch == 'q' || ch == 'Q') break;
        if(ch == 'c' || ch == 'C') edit_config(&state.config);
    }
    endwin();
    return 0;
}
