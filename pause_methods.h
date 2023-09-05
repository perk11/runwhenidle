//
// Created by perk11 on 9/5/23.
//

#ifndef RUNWHENIDLE_PAUSE_METHODS_H
#define RUNWHENIDLE_PAUSE_METHODS_H

enum pause_method {
    //order must match order in pause_method_string
    PAUSE_METHOD_SIGTSTP = 1,
    PAUSE_METHOD_SIGSTOP = 2,
};

extern const char *pause_method_string[];
extern enum pause_method pause_method;
#endif //RUNWHENIDLE_PAUSE_METHODS_H
