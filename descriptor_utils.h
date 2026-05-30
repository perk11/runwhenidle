//
// Created by perk11 on 5/29/26.
//

#ifndef RUNWHENIDLE_DESCRIPTOR_UTILS_H
#define RUNWHENIDLE_DESCRIPTOR_UTILS_H

int create_one_shot_timer_file_descriptor_after_ms(long delay_ms);
int create_periodic_timer_file_descriptor_every_ms(long interval_ms);
void close_file_descriptor_if_open(int *file_descriptor, const char *description);
int consume_timer_file_descriptor_checked(int timer_file_descriptor, const char *description);
#endif //RUNWHENIDLE_DESCRIPTOR_UTILS_H