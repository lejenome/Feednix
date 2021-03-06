#include <curses.h>
#include <filesystem>
#include <map>
#include <panel.h>
#include <menu.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iterator>
#include <algorithm>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <json/json.h>

#include "CursesProvider.h"

#define CTRLD   4
#define POSTS_STATUSLINE "Enter: See Preview  A: mark all read  u: mark unread  r: mark read  = : change sort type s: mark saved  S: mark unsaved R: refresh  o: Open in plain-text  O: Open in Browser  F1: exit"
#define CTG_STATUSLINE "Enter: Fetch Stream  A: mark all read  R: refresh  F1: exit"

#define HOME_PATH getenv("HOME")

namespace fs = std::filesystem;
using namespace std::literals::string_literals;
using PipeStream = std::unique_ptr<FILE, decltype(&pclose)>;

CursesProvider::CursesProvider(const fs::path& tmpPath, bool verbose, bool change):
        feedly{FeedlyProvider(tmpPath)},
        previewPath{tmpPath / "preview.html"}{

        feedly.setVerbose(verbose);
        feedly.setChangeTokensFlag(change);
        feedly.authenticateUser();

        setlocale(LC_ALL, "");
        initscr();

        start_color();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);

        feedly.setVerbose(false);
}
void CursesProvider::init(){
        Json::Value root;
        Json::Reader reader;

        if(const auto browserEnv = getenv("BROWSER")){
                textBrowser = browserEnv;
        }

        std::ifstream tokenFile(std::string(std::string(HOME_PATH) + "/.config/feednix/config.json").c_str(), std::ifstream::binary);
        if(reader.parse(tokenFile, root)){
                init_pair(1, root["colors"]["active_panel"].asInt(), root["colors"]["background"].asInt());
                init_pair(2, root["colors"]["idle_panel"].asInt(), root["colors"]["background"].asInt());
                init_pair(3, root["colors"]["counter"].asInt(), root["colors"]["background"].asInt());
                init_pair(4, root["colors"]["status_line"].asInt(), root["colors"]["background"].asInt());
                init_pair(5, root["colors"]["instructions_line"].asInt(), root["colors"]["background"].asInt());
                init_pair(6, root["colors"]["item_text"].asInt(), root["colors"]["background"].asInt());
                init_pair(7, root["colors"]["item_highlight"].asInt(), root["colors"]["background"].asInt());
                init_pair(8, root["colors"]["read_item"].asInt(), root["colors"]["background"].asInt());

                ctgWinWidth = root["ctg_win_width"].asInt();
                viewWinHeight = root["view_win_height"].asInt();
                viewWinHeightPer = root["view_win_height_per"].asInt();

                currentRank = root["rank"].asBool();
                secondsToMarkAsRead = std::chrono::seconds(root["seconds_to_mark_as_read"].asInt());

                if(textBrowser.empty()){
                        textBrowser = root["text_browser"].asString();
                        if(textBrowser.empty()){
                                textBrowser = "w3m";
                        }
                }
        }
        else{
                endwin();
                feedly.curl_cleanup();
                std::cerr << "ERROR: Couldn't not read config file" << std::endl;
                exit(EXIT_FAILURE);
        }

        if (ctgWinWidth == 0)
                ctgWinWidth = CTG_WIN_WIDTH;
        if (viewWinHeight == 0 && viewWinHeightPer == 0)
                viewWinHeightPer = VIEW_WIN_HEIGHT_PER;
        if (viewWinHeight == 0)
                viewWinHeight = (unsigned int)(((LINES - 2) * viewWinHeightPer) / 100);

        createCategoriesMenu();
        createPostsMenu();

        viewWin = newwin(viewWinHeight, COLS - 2, (LINES - 2 - viewWinHeight), 1);

        panels[0] = new_panel(ctgWin);
        panels[1] = new_panel(postsWin);
        panels[2] = new_panel(viewWin);

        set_panel_userptr(panels[0], panels[1]);
        set_panel_userptr(panels[1], panels[0]);

        printPostMenuMessage("Loading...");
        update_infoline(POSTS_STATUSLINE);

        top = panels[1];
        top_panel(top);

        update_panels();
        doupdate();

        ctgMenuCallback("All");
}
void CursesProvider::control(){
        int ch;
        MENU* curMenu;
        if(totalPosts == 0){
                curMenu = ctgMenu;
        }
        else{
                curMenu = postsMenu;
                changeSelectedItem(curMenu, REQ_FIRST_ITEM);
        }

        while((ch = getch()) != KEY_F(1) && ch != 'q'){
                auto curItem = current_item(curMenu);
                switch(ch){
                        case 10:
                                if((curMenu == ctgMenu) && (curItem != NULL)){
                                        top = (PANEL *)panel_userptr(top);

                                        update_statusline("[Updating stream]", "", false);

                                        refresh();
                                        update_panels();

                                        ctgMenuCallback(item_name(curItem));

                                        top_panel(top);

                                        if(numUnread == 0){
                                                curMenu = ctgMenu;
                                        }
                                        else{
                                                curMenu = postsMenu;
                                                update_infoline(POSTS_STATUSLINE);
                                        }
                                }
                                else if((panel_window(top) == postsWin) && (curItem != NULL)){
                                        postsMenuCallback(curItem, true);
                                }

                                break;
                        case 9:
                                if(curMenu == ctgMenu){
                                        curMenu = postsMenu;

                                        renderWindow(postsWin, "Posts", 1, true);
                                        renderWindow(ctgWin, "Categories", 2, false);

                                        update_infoline(POSTS_STATUSLINE);
                                        refresh();
                                }
                                else{
                                        curMenu = ctgMenu;
                                        renderWindow(ctgWin, "Categories", 1, true);
                                        renderWindow(postsWin, "Posts", 2, false);

                                        update_infoline(CTG_STATUSLINE);

                                        refresh();
                                }

                                top = (PANEL *)panel_userptr(top);
                                top_panel(top);
                                break;
                        case '=':
                                if(auto currentCategoryItem = current_item(ctgMenu)){
                                        wclear(viewWin);
                                        update_statusline("[Updating stream]", "", false);
                                        refresh();

                                        currentRank = !currentRank;

                                        ctgMenuCallback(item_name(currentCategoryItem));
                                }

                                break;
                        case KEY_DOWN:
                                changeSelectedItem(curMenu, REQ_DOWN_ITEM);
                                break;
                        case KEY_UP:
                                changeSelectedItem(curMenu, REQ_UP_ITEM);
                                break;
                        case 'j':
                                changeSelectedItem(curMenu, REQ_DOWN_ITEM);
                                break;
                        case 'k':
                                changeSelectedItem(curMenu, REQ_UP_ITEM);
                                break;
                        case 'u':
                                if((curMenu == postsMenu) && (curItem != NULL) && !item_opts(curItem)){
                                        update_statusline("[Marking post unread]", NULL, true);
                                        refresh();

                                        std::string errorMessage;
                                        try{
                                                feedly.markPostsUnread({item_description(curItem)});

                                                item_opts_on(curItem, O_SELECTABLE);
                                                numUnread++;
                                        }
                                        catch(const std::exception& e){
                                                errorMessage = e.what();
                                        }

                                        update_statusline(errorMessage.c_str(), NULL, errorMessage.empty());

                                        // Prevent an article marked as unread explicitly
                                        // from being marked as read automatically.
                                        lastPostSelectionTime = std::chrono::time_point<std::chrono::steady_clock>::max();
                                }

                                break;
                        case 'r':
                                if((curMenu == postsMenu) && (curItem != NULL)){
                                        markItemRead(curItem);
                                }

                                break;
                        case 's':
                                if((curMenu == postsMenu) && (curItem != NULL)){
                                        update_statusline("[Marking post saved]", NULL, true);
                                        refresh();

                                        std::string errorMessage;
                                        try{
                                                feedly.markPostsSaved({item_description(curItem)});
                                        }
                                        catch(const std::exception& e){
                                                errorMessage = e.what();
                                        }

                                        update_statusline(errorMessage.c_str(), NULL, errorMessage.empty());
                                }

                                break;
                        case 'S':
                                if((curMenu == postsMenu) && (curItem != NULL)){
                                        update_statusline("[Marking post Unsaved]", NULL, true);
                                        refresh();

                                        std::string errorMessage;
                                        try{
                                                feedly.markPostsUnsaved({item_description(curItem)});
                                        }
                                        catch(const std::exception& e){
                                                errorMessage = e.what();
                                        }

                                        update_statusline(errorMessage.c_str(), NULL, errorMessage.empty());
                                }

                                break;
                        case 'R':
                                if(auto currentCategoryItem = current_item(ctgMenu)){
                                        wclear(viewWin);
                                        update_statusline("[Updating stream]", "", false);
                                        refresh();

                                        ctgMenuCallback(item_name(currentCategoryItem));
                                }

                                break;
                        case 'o':
                                if((curMenu == postsMenu) && (curItem != NULL)){
                                        postsMenuCallback(curItem, false);
                                }

                                break;
                        case 'O':
                                if((curMenu == postsMenu) && (curItem != NULL)){
                                        termios oldt;
                                        tcgetattr(STDIN_FILENO, &oldt);
                                        termios newt = oldt;
                                        newt.c_lflag &= ~ECHO;
                                        tcsetattr(STDIN_FILENO, TCSANOW, &newt);

                                        try{
                                                PostData& data = feedly.getSinglePostData(item_index(curItem));
#ifdef __APPLE__
                                                system(std::string("open \"" + data.originURL + "\" > /dev/null &").c_str());
#else
                                                system(std::string("xdg-open \"" + data.originURL + "\" > /dev/null &").c_str());
#endif
                                                markItemRead(curItem);
                                        }
                                        catch(const std::exception& e){
                                                update_statusline(e.what(), NULL /*post*/, false /*showCounter*/);
                                        }
                                }

                                break;
                        case 'a':
                                {
                                        char feed[200];
                                        char title[200];
                                        char ctg[200];
                                        echo();

                                        clear_statusline();
                                        attron(COLOR_PAIR(4));
                                        mvprintw(LINES - 2, 0, "[ENTER FEED]:");
                                        mvgetnstr(LINES-2, strlen("[ENTER FEED]") + 1, feed, 200);
                                        mvaddch(LINES-2, 0, ':');

                                        clrtoeol();

                                        mvprintw(LINES - 2, 0, "[ENTER TITLE]:");
                                        mvgetnstr(LINES-2, strlen("[ENTER TITLE]") + 1, title, 200);
                                        mvaddch(LINES-2, 0, ':');

                                        clrtoeol();

                                        mvprintw(LINES - 2, 0, "[ENTER CATEGORY]:");
                                        mvgetnstr(LINES-2, strlen("[ENTER CATEGORY]") + 1, ctg, 200);
                                        mvaddch(LINES-2, 0, ':');

                                        std::istringstream ss(ctg);
                                        std::istream_iterator<std::string> begin(ss), end;

                                        std::vector<std::string> arrayTokens(begin, end);

                                        noecho();
                                        clrtoeol();

                                        update_statusline("[Adding subscription]", NULL, true);
                                        refresh();

                                        std::string errorMessage;
                                        if(strlen(feed) != 0){
                                                try{
                                                        feedly.addSubscription(false, feed, arrayTokens, title);
                                                }
                                                catch(const std::exception& e){
                                                        errorMessage = e.what();
                                                }
                                        }

                                        update_statusline(errorMessage.c_str(), NULL, errorMessage.empty());
                                }

                                break;
                        case 'A':
                                if(auto currentCategoryItem = current_item(ctgMenu)){
                                        wclear(viewWin);
                                        update_statusline("[Marking category read]", "", true);
                                        refresh();

                                        std::string errorMessage;
                                        try{
                                                feedly.markCategoriesRead(item_description(currentCategoryItem), lastEntryRead);
                                        }
                                        catch(const std::exception& e){
                                                update_statusline(errorMessage.c_str(), NULL, errorMessage.empty());
                                        }

                                        ctgMenuCallback(item_name(currentCategoryItem));
                                        curMenu = ctgMenu;
                                }

                                break;
                }

                update_panels();
                doupdate();
        }

        markItemReadAutomatically(current_item(postsMenu));
}
void CursesProvider::createCategoriesMenu(){
        clearCategoryItems();
        try{
                const auto& labels = feedly.getLabels();
                ctgItems.push_back(new_item("All", labels.at("All").c_str()));
                ctgItems.push_back(new_item("Saved", labels.at("Saved").c_str()));
                ctgItems.push_back(new_item("Uncategorized", labels.at("Uncategorized").c_str()));
                for(const auto& [label, id] : labels){
                        if((label != "All") && (label != "Saved") && (label != "Uncategorized")){
                                ctgItems.push_back(new_item(label.c_str(), id.c_str()));
                        }
                }
        }
        catch(const std::exception& e){
                clearCategoryItems();
                update_statusline(e.what(), NULL /*post*/, false /*showCounter*/);
        }

        ctgItems.push_back(NULL);
        ctgMenu = new_menu(ctgItems.data());

        const auto ctgWinHeight = LINES - 2 - viewWinHeight;
        ctgWin = newwin(ctgWinHeight, ctgWinWidth, 0, 0);
        ctgMenuWin = derwin(ctgWin, (ctgWinHeight - 4), (ctgWinWidth - 2), 3, 1);
        keypad(ctgWin, TRUE);

        set_menu_win(ctgMenu, ctgWin);
        set_menu_sub(ctgMenu, ctgMenuWin);
        set_menu_fore(ctgMenu, COLOR_PAIR(7) | A_REVERSE);
        set_menu_back(ctgMenu, COLOR_PAIR(6));
        set_menu_grey(ctgMenu, COLOR_PAIR(8));
        set_menu_mark(ctgMenu, "  ");

        renderWindow(ctgWin, "Categories", 2, false);

        menu_opts_off(ctgMenu, O_SHOWDESC);
        menu_opts_on(ctgMenu, O_NONCYCLIC);

        post_menu(ctgMenu);
}
void CursesProvider::createPostsMenu(){
        const auto height = LINES - 2 - viewWinHeight;
        postsWin = newwin(height, 0, 0, ctgWinWidth);
        keypad(postsWin, TRUE);

        const auto width = getmaxx(postsWin);
        postsMenuWin = derwin(postsWin, height - 4, width - 2, 3, 1);

        postsMenu = new_menu(NULL);
        set_menu_win(postsMenu, postsWin);
        set_menu_sub(postsMenu, postsMenuWin);
        set_menu_fore(postsMenu, COLOR_PAIR(7) | A_REVERSE);
        set_menu_back(postsMenu, COLOR_PAIR(6));
        set_menu_grey(postsMenu, COLOR_PAIR(8));
        set_menu_mark(postsMenu, "*");

        renderWindow(postsWin, "Posts", 1, true);

        menu_opts_off(postsMenu, O_SHOWDESC);

        post_menu(postsMenu);
}
void CursesProvider::ctgMenuCallback(const char* label){
        markItemReadAutomatically(current_item(postsMenu));

        int startx, height, width;
        [[maybe_unused]] int starty;

        getmaxyx(postsWin, height, width);
        getbegyx(postsWin, starty, startx);

        std::string errorMessage;
        clearPostItems();
        try{
                const auto& posts = feedly.giveStreamPosts(label, currentRank);
                for(const auto& post : posts){
                        postsItems.push_back(new_item(post.title.c_str(), post.id.c_str()));
                }
        }
        catch(const std::exception& e){
                clearPostItems();
                errorMessage = e.what();
        }

        totalPosts = postsItems.size();
        numUnread = totalPosts;
        printPostMenuMessage("");

        postsItems.push_back(NULL);
        unpost_menu(postsMenu);
        set_menu_items(postsMenu, postsItems.data());
        set_menu_format(postsMenu, height - 4, 0);
        post_menu(postsMenu);

        update_statusline(errorMessage.c_str(), NULL, errorMessage.empty());
        renderWindow(postsWin, "Posts", 1, true);
        renderWindow(ctgWin, "Categories", 2, false);

        if(totalPosts > 0){
                lastEntryRead = item_description(postsItems.at(0));
                changeSelectedItem(postsMenu, REQ_FIRST_ITEM);
        }
        else
        {
                printPostMenuMessage("All Posts Read");
                wclear(viewWin);
        }
}
void CursesProvider::changeSelectedItem(MENU* curMenu, int req){
        ITEM* previousItem = current_item(curMenu);
        menu_driver(curMenu, req);
        ITEM* curItem = current_item(curMenu);

        if((curMenu != postsMenu) ||
            !curItem ||
            ((previousItem == curItem) && (req != REQ_FIRST_ITEM))){
                return;
        }

        markItemReadAutomatically(previousItem);

        try{
                const auto& postData = feedly.getSinglePostData(item_index(curItem));
                if(auto myfile = std::ofstream(previewPath.c_str())){
                        myfile << postData.content;
                }

                std::string content;
                char buffer[256];
                const auto command = "w3m -dump -cols " + std::to_string(COLS - 2) + " " + previewPath.native();
                if(const auto stream = PipeStream(popen(command.c_str(), "r"), &pclose)){
                        while(!feof(stream.get())){
                                if(fgets(buffer, 256, stream.get()) != NULL){
                                        content.append(buffer);
                                }
                        }
                }

                wclear(viewWin);
                mvwprintw(viewWin, 1, 1, content.c_str());
                wrefresh(viewWin);
                update_statusline(NULL, (postData.originTitle + " - " + postData.title).c_str(), true);

                update_panels();
        }
        catch (const std::exception& e){
                update_statusline(e.what(), NULL /*post*/, false /*showCounter*/);
        }
}
void CursesProvider::postsMenuCallback(ITEM* item, bool preview){
        auto command = std::string{};
        try{
                const auto& postData = feedly.getSinglePostData(item_index(item));
                if(preview){
                        if(auto myfile = std::ofstream(previewPath.c_str())){
                                myfile << postData.content;
                        }

                        command = "w3m " + previewPath.native();
                }
                else{
                        command = textBrowser + " \'" + postData.originURL + "\'";
                }

        }
        catch (const std::exception& e){
                update_statusline(e.what(), NULL /*post*/, false /*showCounter*/);
                return;
        }

        def_prog_mode();
        endwin();
        const auto exitCode = system(command.c_str());
        reset_prog_mode();

        if(exitCode == 0){
                markItemRead(item);
                lastEntryRead = item_description(item);
        }
        else{
                const auto updateStatus = preview ? "Failed to preview the post" : "Failed to open the post";
                update_statusline(updateStatus, NULL /*post*/, false /*showCounter*/);
        }

        if(preview){
                auto errorCode = std::error_code{};
                fs::remove(previewPath, errorCode);
        }
}
void CursesProvider::markItemRead(ITEM* item){
        if(item_opts(item)){
                item_opts_off(item, O_SELECTABLE);
                update_statusline("[Marking post read]", NULL, true);
                refresh();

                std::string errorMessage;
                try{
                        const auto& postData = feedly.getSinglePostData(item_index(item));
                        feedly.markPostsRead({postData.id});
                        numUnread--;
                }
                catch (const std::exception& e){
                        errorMessage = e.what();
                }

                update_statusline(errorMessage.c_str(), NULL, errorMessage.empty());
                update_panels();
        }
}
// Mark an article as read if it has been shown for more than a certain period of time.
void CursesProvider::markItemReadAutomatically(ITEM* item){
        const auto now = std::chrono::steady_clock::now();
        if ((item != NULL) &&
            (now > lastPostSelectionTime) &&
            (secondsToMarkAsRead >= std::chrono::seconds::zero()) &&
            ((now - lastPostSelectionTime) > secondsToMarkAsRead)){
                markItemRead(item);
        }

        lastPostSelectionTime = now;
}
void CursesProvider::renderWindow(WINDOW *win, const char *label, int labelColor, bool highlight){
        int startx, width;
        [[maybe_unused]] int starty, height;

        getbegyx(win, starty, startx);
        getmaxyx(win, height, width);

        mvwaddch(win, 2, 0, ACS_LTEE);
        mvwhline(win, 2, 1, ACS_HLINE, width - 2);
        mvwaddch(win, 2, width - 1, ACS_RTEE);

        if(highlight){
                wattron(win, COLOR_PAIR(labelColor));
                box(win, 0, 0);
                mvwaddch(win, 2, 0, ACS_LTEE);
                mvwhline(win, 2, 1, ACS_HLINE, width - 2);
                mvwaddch(win, 2, width - 1, ACS_RTEE);
                printInMiddle(win, 1, 0, width, label, COLOR_PAIR(labelColor));
                wattroff(win, COLOR_PAIR(labelColor));
        }
        else{
                wattron(win, COLOR_PAIR(2));
                box(win, 0, 0);
                mvwaddch(win, 2, 0, ACS_LTEE);
                mvwhline(win, 2, 1, ACS_HLINE, width - 2);
                mvwaddch(win, 2, width - 1, ACS_RTEE);
                printInMiddle(win, 1, 0, width, label, COLOR_PAIR(5));
                wattroff(win, COLOR_PAIR(2));
        }

}
void CursesProvider::printInMiddle(WINDOW *win, int starty, int startx, int width, const char *str, chtype color){
        int length, x, y;
        float temp;

        if(win == NULL)
                win = stdscr;
        getyx(win, y, x);
        if(startx != 0)
                x = startx;
        if(starty != 0)
                y = starty;
        if(width == 0)
                width = 80;

        length = strlen(str);
        temp = (width - length)/ 2;
        x = startx + (int)temp;
        mvwprintw(win, y, x, "%s", str);
}
void CursesProvider::printPostMenuMessage(const std::string& message){
        const auto height = getmaxy(postsMenuWin);
        const auto width = getmaxx(postsMenuWin);
        const auto y = height / 2;
        const auto x = (width - message.length()) / 2;

        werase(postsMenuWin);
        wattron(postsMenuWin, 1);
        mvwprintw(postsMenuWin, y, x, message.c_str());
        wattroff(postsMenuWin, 1);
}
void CursesProvider::clear_statusline(){
        move(LINES-2, 0);
        clrtoeol();
}
void CursesProvider::update_statusline(const char* update, const char* post, bool showCounter){
        if (update != NULL)
                statusLine[0] = std::string(update);
        if (post != NULL)
                statusLine[1] = std::string(post);
        if (showCounter) {
                const auto numRead = totalPosts - numUnread;
                std::stringstream sstm;
                sstm << "[" << numUnread << ":" << numRead << "/" << totalPosts << "]";
                statusLine[2] = sstm.str();
        } else {
                statusLine[2] = std::string();
        }

        clear_statusline();
        move(LINES - 2, 0);
        clrtoeol();
        attron(COLOR_PAIR(1));
        mvprintw(LINES - 2, 0, statusLine[0].c_str());
        attroff(COLOR_PAIR(1));
        mvprintw(LINES - 2, statusLine[0].empty() ? 0 : (statusLine[0].length() + 1), statusLine[1].substr(0,
                                COLS - statusLine[0].length() - statusLine[2].length() - 2).c_str());
        attron(COLOR_PAIR(3));
        mvprintw(LINES - 2, COLS - statusLine[2].length(), statusLine[2].c_str());
        attroff(COLOR_PAIR(3));
        refresh();
        update_panels();
}
void CursesProvider::update_infoline(const char* info){
        move(LINES-1, 0);
        clrtoeol();
        attron(COLOR_PAIR(5));
        mvprintw(LINES - 1, 0, info);
        attroff(COLOR_PAIR(5));
}
void CursesProvider::clearCategoryItems(){
        for(const auto& ctgItem : ctgItems){
                if(ctgItem != NULL){
                        free_item(ctgItem);
                }
        }

        ctgItems.clear();
}
void CursesProvider::clearPostItems(){
        for(const auto& postItem : postsItems){
                if(postItem != NULL){
                        free_item(postItem);
                }
        }

        postsItems.clear();
}
CursesProvider::~CursesProvider(){
        if(ctgMenu != NULL){
                unpost_menu(ctgMenu);
                free_menu(ctgMenu);
        }

        if(postsMenu != NULL){
                unpost_menu(postsMenu);
                free_menu(postsMenu);
        }

        clearCategoryItems();
        clearPostItems();
        endwin();
        feedly.curl_cleanup();
}
