#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>

#define EVENT_SIZE 4
#define COUNT 256
#define HEADER_LENGTH 14

struct midi_track
{
    off_t track_offset;
    size_t curr_tick;
    uint8_t event[EVENT_SIZE];
    uint8_t is_meta;
    uint8_t queued;
    uint8_t stopped;
};

int main(void)
{
    int device = open("/dev/midi0", O_WRONLY);
    int fd = open("./hammerklavier-fixed.mid", O_RDONLY);
    uint8_t buffer[COUNT];
    uint8_t header[HEADER_LENGTH];
    uint16_t number_of_tracks;
    uint16_t ticks_per_beat;
    read(fd, header, HEADER_LENGTH);
    if (memcmp(header, "MThd", 4) != 0)
        return 1;
    number_of_tracks = (header[10] << 8) | header[11];
    ticks_per_beat = (header[12] << 8) | header[13];
    struct midi_track *tracks = calloc(number_of_tracks, sizeof(struct midi_track));
    for (size_t i = 0; i < number_of_tracks; ++i)
    {
        off_t track_start = lseek(fd, 0, SEEK_CUR);
        read(fd, header, 8);
        if (memcmp(header, "MTrk", 4) != 0) break;
        size_t track_length = header[4] << 24 | header[5] << 16 | header[6] << 8 | header[7];
        tracks[i].track_offset = track_start+8;
        lseek(fd, track_length, SEEK_CUR);
    }
    size_t tempo = 0;
    size_t global_tick = 0;
    while (!tracks[0].stopped)
    {
        size_t ind;
        for (ind = 0; ind < number_of_tracks; ++ind)
        {
            if (tracks[ind].queued) continue;
            lseek(fd, tracks[ind].track_offset, SEEK_SET);
            size_t delta = 0;
            off_t offset = 0;
            uint8_t byte;
            do
            {
                read(fd, &byte, 1);
                delta = (delta << 7) | (byte & 0x7F);
                offset++;
            }
            while (byte & 0x80);
            tracks[ind].curr_tick += delta;
            read(fd, &byte, 1);
            offset++;
            size_t bytes_for_event;
            uint8_t type;
            switch (byte & 0xF0)
            {
                case 0xC0:
                    bytes_for_event = 1;
                    break;
                case 0xD0:
                    bytes_for_event = 1;
                    break;
                case 0xF0:
                    bytes_for_event = 0;
                    break;
                default:
                    bytes_for_event = 2;
                    break;
            }
            offset += bytes_for_event;
            if (!bytes_for_event)
            {
                tracks[ind].is_meta = 1;
                read(fd, &type, 1);
                offset++;
                do
                {
                    read(fd, &byte, 1);
                    bytes_for_event = (bytes_for_event << 7) | (byte & 0x7F);
                    offset++;
                }
                while (byte & 0x80);
                offset += bytes_for_event;
                read(fd, buffer, bytes_for_event);
                switch (type)
                {
                    case 0x51:
                        tempo = 0;
                        for (size_t i = 0; i < bytes_for_event; ++i)
                            tempo = (tempo << 8) | buffer[i];
                        break;
                    case 0x58:
                        break;
                    case 0x03:
                        break;
                    case 0x2F:
                        tracks[ind].stopped = 1;
                        if (ind != number_of_tracks-1)
                        {
                            tracks[ind] = tracks[number_of_tracks-1];
                        }
                        number_of_tracks--;
                        break;
                }
            }
            else
            {
                tracks[ind].is_meta = 0;
                read(fd, buffer, bytes_for_event);
                uint8_t usb_byte = 0;
                usb_byte |= (byte & 0xF0) >> 4;
                tracks[ind].event[0] = usb_byte;
                tracks[ind].event[1] = byte;
                memcpy(tracks[ind].event+2, buffer, bytes_for_event);
            }
            tracks[ind].track_offset += offset;
            tracks[ind].queued = 1;
        }
        size_t min_ind = 0;
        for (ind = 1; ind < number_of_tracks; ++ind)
        {
            if (tracks[ind].curr_tick < tracks[min_ind].curr_tick)
                min_ind = ind;
        }
        size_t curr_delta = tracks[min_ind].curr_tick-global_tick;
        global_tick = tracks[min_ind].curr_tick;
        if (!tracks[min_ind].is_meta)
        {
            if (curr_delta == 0)
            {
                write(device, tracks[min_ind].event, EVENT_SIZE);
            }
            else
            {
                double micro = (double)tempo*((double)curr_delta/(double)ticks_per_beat);
                usleep(micro);
                write(device, tracks[min_ind].event, EVENT_SIZE);
            }
        }
        tracks[min_ind].queued = 0;
    }
    free(tracks);
    close(fd);
    close(device);

    return 0;
}
