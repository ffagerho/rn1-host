#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323
#endif

#include "datatypes.h"
#include "map_memdisk.h"
#include "mapping.h"

static const int search_order[25][2] = { 
	{ 0, 0},
	{ 0, 1},
	{ 0,-1},
	{ 1, 0},
	{-1, 0},
	{ 1, 1},
	{ 1,-1},
	{-1, 1},
	{-1,-1},
	{ 0, 2},
	{ 0,-2},
	{ 1, 2},
	{ 1,-2},
	{-1, 2},
	{-1,-2},
	{ 2, 0},
	{-2, 0},
	{ 2, 1},
	{ 2,-1},
	{-2, 1},
	{-2,-1},
	{ 2, 2},
	{ 2,-2},
	{-2, 2},
	{-2,-2}};


world_t world;

void page_coords(int mm_x, int mm_y, int* pageidx_x, int* pageidx_y, int* pageoffs_x, int* pageoffs_y)
{
	int unit_x = mm_x / MAP_UNIT_W;
	int unit_y = mm_y / MAP_UNIT_W;
	unit_x += MAP_MIDDLE_UNIT;
	unit_y += MAP_MIDDLE_UNIT;
	int page_x = unit_x / MAP_PAGE_W;
	int page_y = unit_y / MAP_PAGE_W;
	int offs_x = unit_x - page_x*MAP_PAGE_W;
	int offs_y = unit_y - page_y*MAP_PAGE_W;

	*pageidx_x = page_x;
	*pageidx_y = page_y;
	*pageoffs_x = offs_x;
	*pageoffs_y = offs_y;
}

void unit_coords(int mm_x, int mm_y, int* unit_x, int* unit_y)
{
	int unit_x_t = mm_x / MAP_UNIT_W;
	int unit_y_t = mm_y / MAP_UNIT_W;
	unit_x_t += MAP_MIDDLE_UNIT;
	unit_y_t += MAP_MIDDLE_UNIT;

	*unit_x = unit_x_t;
	*unit_y = unit_y_t;
}

void mm_from_unit_coords(int unit_x, int unit_y, int* mm_x, int* mm_y)
{
	unit_x -= MAP_MIDDLE_UNIT;
	unit_y -= MAP_MIDDLE_UNIT;

	*mm_x = unit_x * MAP_UNIT_W;
	*mm_y = unit_y * MAP_UNIT_W;
}

void page_coords_from_unit_coords(int unit_x, int unit_y, int* pageidx_x, int* pageidx_y, int* pageoffs_x, int* pageoffs_y)
{
	int page_x = unit_x / MAP_PAGE_W;
	int page_y = unit_y / MAP_PAGE_W;
	int offs_x = unit_x - page_x*MAP_PAGE_W;
	int offs_y = unit_y - page_y*MAP_PAGE_W;

	*pageidx_x = page_x;
	*pageidx_y = page_y;
	*pageoffs_x = offs_x;
	*pageoffs_y = offs_y;
}


// Shift page,offset coords directly by shift_x, shift_y units.
void shift_coords(int* px, int* py, int* ox, int* oy, int shift_x, int shift_y)
{
	*ox += shift_x;
	while(*ox > MAP_PAGE_W)
	{
		*ox -= MAP_PAGE_W;
		(*px)--;
	}
	while(*ox < 0)
	{
		*ox += MAP_PAGE_W;
		(*px)++;
	}

	*oy += shift_x;
	while(*oy > MAP_PAGE_W)
	{
		*oy -= MAP_PAGE_W;
		(*py)--;
	}
	while(*oy < 0)
	{
		*oy += MAP_PAGE_W;
		(*py)++;
	}
}


#define sq(x) ((x)*(x))

/*
	Go through every point in every lidar scan.
	Find closest point from every other scan. If far away,
	remove the point as being a moving object (or some kind of error).

	modifies lidar scans pointed by lidar_list.
*/

static int prefilter_lidar_list(int n_lidars, lidar_scan_t** lidar_list)
{
	int n_removed_per_scan[32] = {0};
	int n_removed = 0;

	for(int la=0; la<n_lidars; la++)
	{
		lidar_scan_t* lida = lidar_list[la];

		for(int pa=0; pa<LIDAR_SCAN_POINTS; pa++)
		{
			if(!lida->scan[pa].valid)
				continue;

			int64_t closest = INT64_MAX;

			for(int lb=0; lb<n_lidars; lb++)
			{
				if(la == lb) continue;

				lidar_scan_t* lidb = lidar_list[lb];

				for(int pb=0; pb<LIDAR_SCAN_POINTS; pb++)
				{
					if(!lidb->scan[pb].valid)
						continue;

					int64_t dx = lidb->scan[pb].x - lida->scan[pa].x;
					int64_t dy = lidb->scan[pb].y - lida->scan[pa].y;
					int64_t dist = sq(dx) + sq(dy);

					if(dist < closest) closest = dist;
				}
			}

			if(closest > sq(100))
			{
				lida->scan[pa].valid = 0;
				n_removed_per_scan[la]++;
				n_removed++;
			}


		}
	}

	printf("INFO: prefilter_lidar_list() removed %d points: ", n_removed);
	for(int i=0; i < n_lidars; i++)	printf("%d, ", n_removed_per_scan[i]);
	printf("\n");
	return n_removed;
}

/* Rotation:
x2 = x*cos(a) + y*sin(a)
y2 = -1*x*sin(a) + y*cos(a)
*/

static int score(world_t* w, int n_lidars, lidar_scan_t** lidar_list, 
	         int32_t da, int32_t dx, int32_t dy, int32_t rotate_mid_x, int32_t rotate_mid_y,
	         int* n_matched_walls, int* n_exactly_matched_walls, int* n_new_walls, int* n_discovered_walls)
{
	int pagex, pagey, offsx, offsy;

	int n_points = 0;
	int n_matches = 0;
	int n_news = 0;
	int n_exacts = 0;
	int n_steadys = 0;

	// Go through all valid points in all lidars in the lidar_list.
	for(int l=0; l<n_lidars; l++)
	{
		lidar_scan_t* lid = lidar_list[l];

		// 9 pages will cover the full lidar scan by design.
		page_coords(lid->robot_pos.x, lid->robot_pos.y, &pagex, &pagey, &offsx, &offsy);
		load_9pages(&world, pagex, pagey);
		

		for(int p=0; p<LIDAR_SCAN_POINTS; p++)
		{
			if(!lid->scan[p].valid)
				continue;

			n_points++;

			// Rotate the point by da and then shift by dx, dy.
			float ang = (float)da/((float)ANG_1_DEG*360.0)*2.0*M_PI;
	
			int pre_x = lid->scan[p].x - rotate_mid_x;
			int pre_y = lid->scan[p].y - rotate_mid_y;

			int x = pre_x*cos(ang) + pre_y*sin(ang) + rotate_mid_x + dx;
			int y = -1*pre_x*sin(ang) + pre_y*cos(ang) + rotate_mid_y + dy;

			int is_match = 0;
			int seen_with_no_wall = 0;
			int is_exact = 0;
			int is_steady = 0; // There is a wall in the decided result field.

			// Wall in any neighbouring cell is considered a match.

			page_coords(x, y, &pagex, &pagey, &offsx, &offsy);
			if(w->pages[pagex][pagey]->units[offsx][offsy].result & UNIT_WALL)
			{
				is_match = 1;
				is_exact = 1;
				is_steady = 1;
			}
			else if(w->pages[pagex][pagey]->units[offsx][offsy].num_obstacles)
			{
				is_match = 1;
				is_exact = 1;
			}
			else
			{
				for(int ix=-1*MAP_UNIT_W; ix<=MAP_UNIT_W; ix+=MAP_UNIT_W)
				{
					for(int iy=-1*MAP_UNIT_W; iy<=MAP_UNIT_W; iy+=MAP_UNIT_W)
					{
						page_coords(x+ix, y+iy, &pagex, &pagey, &offsx, &offsy);
						if(w->pages[pagex][pagey]->units[offsx][offsy].result & UNIT_WALL)
						{
							is_match = 1;
							is_steady = 1;
							break;
						}
						else if(w->pages[pagex][pagey]->units[offsx][offsy].num_obstacles)
						{
							is_match = 1;
						}
						else if(w->pages[pagex][pagey]->units[offsx][offsy].result & UNIT_MAPPED)
						{
							seen_with_no_wall++;
						}
					}
				}
			}

			if(is_match) n_matches++;
			if(is_exact) n_exacts++;
			if(is_steady) n_steadys++;
			// There is no wall, and most of the 9 units were mapped "no wall" before, so we have a new wall:
			if(!is_match && seen_with_no_wall > 6) n_news++;

			
			
		}
	}

	// Write the results
	if(n_matched_walls) *n_matched_walls = n_matches;
	if(n_exactly_matched_walls) *n_exactly_matched_walls = n_exacts;
	if(n_new_walls) *n_new_walls = n_news;
	if(n_discovered_walls) *n_discovered_walls = n_points - n_matches;

	// Return the score: bigger = better
	// Exact matches have a slight effect on the result.
	// New walls decrease the score.
	return n_matches*5 + n_steadys*3 + n_exacts*1 - n_news*5;
}

typedef struct  // Each bit represents each lidar scan (i.e., 32 lidar scans max).
{
	uint32_t seen;
	uint32_t wall;
} temp_map_img_t;

#define TEMP_MAP_W (3*MAP_PAGE_W)
#define TEMP_MAP_MIDDLE (TEMP_MAP_W/2)

#define PLUS_SAT_255(x) {if((x)<255) (x)++;}
#define MINUS_SAT_0(x) {if((x)>0) (x)--;}

static int do_mapping(world_t* w, int n_lidars, lidar_scan_t** lidar_list,
                      int32_t da, int32_t dx, int32_t dy, int32_t rotate_mid_x, int32_t rotate_mid_y,
                      int32_t *after_dx, int32_t *after_dy)
{
	int pagex, pagey, offsx, offsy;

	*after_dx = 0;
	*after_dy = 0;

	/*
		Generate temporary map, counting seen areas / wall areas.
		This map is (3* MAP_PAGE_W) x (3* MAP_PAGE_W) in size, the middle point being at rotate_mid_x,rotate_mid_y.
	*/

	temp_map_img_t* temp_map = calloc(3*MAP_PAGE_W*3*MAP_PAGE_W, sizeof(temp_map_img_t));
	if(!temp_map)
	{
		printf("ERROR: Out of memory in do_mapping\n");
		return -1;
	}

	// Go through all valid points in all lidars in the lidar_list.
	for(int l=0; l<n_lidars; l++)
	{
		lidar_scan_t* lid = lidar_list[l];

		// Rotate the point by da and then shift by dx, dy.
		float ang = (float)da/((float)ANG_1_DEG*360.0)*2.0*M_PI;

		int robot_pre_x = lid->robot_pos.x - rotate_mid_x;
		int robot_pre_y = lid->robot_pos.y - rotate_mid_y;

		int robot_x = robot_pre_x*cos(ang) + robot_pre_y*sin(ang) /* + rotate_mid_x */ + dx ;
		int robot_y = -1*robot_pre_x*sin(ang) + robot_pre_y*cos(ang) /* + rotate_mid_y */ + dy;

		robot_x /= MAP_UNIT_W; robot_y /= MAP_UNIT_W;
		robot_x += TEMP_MAP_MIDDLE; robot_y += TEMP_MAP_MIDDLE;

		// Robot coords should be in the middle 1/3..2/3 section.
		if(robot_x < MAP_PAGE_W || robot_x >= 2*MAP_PAGE_W || robot_y < MAP_PAGE_W || robot_y > 2*MAP_PAGE_W)
		{
			printf("ERROR: out of range temp map coords (%d, %d) (robot position)\n", robot_x, robot_y);
			free(temp_map);
			return -2;
		}
		
		for(int p=0; p<LIDAR_SCAN_POINTS; p++)
		{
			if(!lid->scan[p].valid)
				continue;

			// Rotate the point by da and then shift by dx, dy.	
			int pre_x = lid->scan[p].x - rotate_mid_x;
			int pre_y = lid->scan[p].y - rotate_mid_y;

			int x = pre_x*cos(ang) + pre_y*sin(ang) /* + rotate_mid_x */ + dx ;
			int y = -1*pre_x*sin(ang) + pre_y*cos(ang) /* + rotate_mid_y */ + dy;

			x /= MAP_UNIT_W; y /= MAP_UNIT_W;

			x += TEMP_MAP_MIDDLE; y += TEMP_MAP_MIDDLE;

			if(x < 0 || x >= 3*MAP_PAGE_W || y < 0 || y > 3*MAP_PAGE_W)
			{
				printf("ERROR: out of range temp map coords (%d, %d) (scan point)\n", x, y);
				free(temp_map);
				return -2;
			}

			// Mark areas between the robot coords and the current point: "seen".

			int dx = x - robot_x;
			int dy = y - robot_y;

			if(abs(dx) >= abs(dy)) // Step in X direction
			{
				if(dx >= 0)
				{
					float dy_per_step = (float)dy/(float)dx;
					for(int ix = 0; ix < dx; ix++)
					{
						int cur_y = robot_y + dy_per_step*(float)ix;
						int cur_x = robot_x + ix;
						temp_map[cur_y*TEMP_MAP_W + cur_x].seen |= 1UL<<l;
					}
				}
				else // dx < 0
				{
					float dy_per_step = (float)dy/(float)dx;
					for(int ix = 0; ix < -1*dx; ix++)
					{
						int cur_y = robot_y - dy_per_step*(float)ix;
						int cur_x = robot_x - ix;
						temp_map[cur_y*TEMP_MAP_W + cur_x].seen |= 1UL<<l;
					}
				}

			}
			else // Step in Y direction
			{
				if(dy >= 0)
				{
					float dx_per_step = (float)dx/(float)dy;
					for(int iy = 0; iy < dy; iy++)
					{
						int cur_x = robot_x + dx_per_step*(float)iy;
						int cur_y = robot_y + iy;
						temp_map[cur_y*TEMP_MAP_W + cur_x].seen |= 1UL<<l;
					}
				}
				else // dy < 0
				{
					float dx_per_step = (float)dx/(float)dy;
					for(int iy = 0; iy < -1*dy; iy++)
					{
						int cur_x = robot_x - dx_per_step*(float)iy;
						int cur_y = robot_y - iy;
						temp_map[cur_y*TEMP_MAP_W + cur_x].seen |= 1UL<<l;
					}
				}

			}

			// Finally, mark the lidar point as a wall, at the end of the "seen" vector
			temp_map[y*TEMP_MAP_W + x].wall |= 1UL<<l;
		}
	}


	// Output 768x768x24bit raw image for debug.
	FILE* dbg_f = fopen("dbg_image_before.data", "w");

	for(int iy = 0; iy < TEMP_MAP_W; iy++)
	{
		for(int ix = 0; ix < TEMP_MAP_W; ix++)
		{
			int s_cnt = 0, w_cnt = 0;
			uint32_t tmp = temp_map[iy*TEMP_MAP_W+ix].seen;
			while(tmp)
			{
				s_cnt++;
				tmp>>=1;
			}
			tmp = temp_map[iy*TEMP_MAP_W+ix].wall;
			while(tmp)
			{
				w_cnt++;
				tmp>>=1;
			}

			s_cnt*=20;
			w_cnt*=20;

			fputc(w_cnt, dbg_f); // R
			fputc(s_cnt, dbg_f); // G
			fputc(0, dbg_f);
		}
	}

	fclose(dbg_f);

	/*
		Processing round - try to remove duplicate wall units within the same vectors.
	*/

	for(int l=0; l<n_lidars; l++)
	{
		lidar_scan_t* lid = lidar_list[l];

		// Rotate the point by da and then shift by dx, dy.
		float ang = (float)da/((float)ANG_1_DEG*360.0)*2.0*M_PI;

		int robot_pre_x = lid->robot_pos.x - rotate_mid_x;
		int robot_pre_y = lid->robot_pos.y - rotate_mid_y;

		int robot_x = robot_pre_x*cos(ang) + robot_pre_y*sin(ang) /* + rotate_mid_x */ + dx ;
		int robot_y = -1*robot_pre_x*sin(ang) + robot_pre_y*cos(ang) /* + rotate_mid_y */ + dy;

		robot_x /= MAP_UNIT_W; robot_y /= MAP_UNIT_W;
		robot_x += TEMP_MAP_MIDDLE; robot_y += TEMP_MAP_MIDDLE;

		for(int p=0; p<LIDAR_SCAN_POINTS; p++)
		{
			if(!lid->scan[p].valid)
				continue;

			// Rotate the point by da and then shift by dx, dy.	
			int pre_x = lid->scan[p].x - rotate_mid_x;
			int pre_y = lid->scan[p].y - rotate_mid_y;

			int x = pre_x*cos(ang) + pre_y*sin(ang) /* + rotate_mid_x */ + dx ;
			int y = -1*pre_x*sin(ang) + pre_y*cos(ang) /* + rotate_mid_y */ + dy;

			x /= MAP_UNIT_W; y /= MAP_UNIT_W;

			x += TEMP_MAP_MIDDLE; y += TEMP_MAP_MIDDLE;

			// Find the next unit from where we did put the wall before.

			int dx = x - robot_x;
			int dy = y - robot_y;

			int next_x, next_y;
			if(abs(dx) >= abs(dy)) // Step in X direction
			{
				float dy_per_step = (float)dy/(float)dx;
				int next_dx = dx + ((dx>0)?1:-1);
				next_y = robot_y + dy_per_step*(float)next_dx;
				next_x = robot_x + next_dx;
			}
			else // Step in Y direction
			{
				float dx_per_step = (float)dx/(float)dy;
				int next_dy = dy + ((dy>0)?1:-1);
				next_x = robot_x + dx_per_step*(float)next_dy;
				next_y = robot_y + next_dy;

			}

			int w_cnt_at_next = 0;
			uint32_t tmp = temp_map[next_y*TEMP_MAP_W+next_x].wall;
			while(tmp) { w_cnt_at_next++; tmp>>=1;}

			int w_cnt_at_cur = 0;
			tmp = temp_map[y*TEMP_MAP_W+x].wall;
			while(tmp) { w_cnt_at_cur++; tmp>>=1;}

			if(w_cnt_at_next > 0 && w_cnt_at_cur > 0 && w_cnt_at_next > w_cnt_at_cur) // next spot wins
			{
				temp_map[next_y*TEMP_MAP_W+next_x].wall |= temp_map[y*TEMP_MAP_W + x].wall; // Mark all hits to the next spot.
				temp_map[y*TEMP_MAP_W + x].wall = 0; // remove the wall from where it was.
			}
			else if(w_cnt_at_cur > 0 && w_cnt_at_next > 0 && w_cnt_at_cur > w_cnt_at_next) // cur pos wins
			{
				temp_map[y*TEMP_MAP_W+x].wall |= temp_map[next_y*TEMP_MAP_W + next_x].wall; // Mark all those hits to the current spot
				temp_map[next_y*TEMP_MAP_W + next_x].wall = 0; // remove the wall from the next spot
			}
		}
	}


	// Output 768x768x24bit raw image for debug.
	dbg_f = fopen("dbg_image_after.data", "w");

	for(int iy = 0; iy < TEMP_MAP_W; iy++)
	{
		for(int ix = 0; ix < TEMP_MAP_W; ix++)
		{
			int s_cnt = 0, w_cnt = 0;
			uint32_t tmp = temp_map[iy*TEMP_MAP_W+ix].seen;
			while(tmp)
			{
				s_cnt++;
				tmp>>=1;
			}
			tmp = temp_map[iy*TEMP_MAP_W+ix].wall;
			while(tmp)
			{
				w_cnt++;
				tmp>>=1;
			}

			s_cnt*=20;
			w_cnt*=20;

			fputc(w_cnt, dbg_f); // R
			fputc(s_cnt, dbg_f); // G
			fputc(0, dbg_f);
		}
	}

	fclose(dbg_f);

	// Load relevant 9 pages in memory
	page_coords(rotate_mid_x, rotate_mid_y, &pagex, &pagey, &offsx, &offsy);
	load_9pages(&world, pagex, pagey);

	// Add our temporary map to the actual map.
	// Don't loop near to the edges, we are comparing neighbouring cells inside the loop.
	// Operate by reading a copy, writing to actual map, so that what we have just now written doesn't affect the adjacent units:

	
	int mid_x_mm = (rotate_mid_x/MAP_UNIT_W)*MAP_UNIT_W;
	int mid_y_mm = (rotate_mid_y/MAP_UNIT_W)*MAP_UNIT_W;
	page_coords(mid_x_mm, mid_y_mm, &pagex, &pagey, &offsx, &offsy);

	static map_page_t copies[3][3];
	static uint8_t spot_used[3][3][MAP_PAGE_W][MAP_PAGE_W];

	int copy_pagex_start = pagex-1;
	int copy_pagey_start = pagey-1;

	for(int i = 0; i<3; i++)
	{
		for(int o=0; o<3; o++)
		{
			memcpy(&copies[i][o], w->pages[copy_pagex_start+i][copy_pagey_start+o], sizeof(map_page_t));
			memset(spot_used[i][o], 0, MAP_PAGE_W*MAP_PAGE_W);
		}
	}

	int avg_drift_cnt = 0, avg_drift_x = 0, avg_drift_y = 0;

	for(int iy = 3; iy < TEMP_MAP_W-3; iy++)
	{
		for(int ix = 3; ix < TEMP_MAP_W-3; ix++)
		{
			int x_mm = (rotate_mid_x/MAP_UNIT_W - TEMP_MAP_MIDDLE + ix)*MAP_UNIT_W;
			int y_mm = (rotate_mid_y/MAP_UNIT_W - TEMP_MAP_MIDDLE + iy)*MAP_UNIT_W;
			page_coords(x_mm, y_mm, &pagex, &pagey, &offsx, &offsy);
//			if(ix == 2 && iy == 2)
//				printf("Info: temp map -> map: start: page (%d, %d) offs (%d, %d)\n", pagex, pagey, offsx, offsy);

//			float ang_from_middle = atan2(y_mm-rotate_mid_y, x_mm-rotete_mid_x)*(8.0/(2.0*M_PI));
//			if(ang_from_middle < 0.0) ang_from_middle += 8.0;
//			int ang_idx = ang_from_middle+0.5;

			int s_cnt = 0, w_cnt = 0, neigh_w_cnt = 0;
			uint32_t tmp = temp_map[iy*TEMP_MAP_W+ix].seen; while(tmp) { s_cnt++; tmp>>=1; }
			tmp = temp_map[iy*TEMP_MAP_W+ix].wall; while(tmp) { w_cnt++; tmp>>=1; }

			tmp = temp_map[(iy)*TEMP_MAP_W+(ix+1)].wall; while(tmp) { neigh_w_cnt++; tmp>>=1; }
			tmp = temp_map[(iy)*TEMP_MAP_W+(ix-1)].wall; while(tmp) { neigh_w_cnt++; tmp>>=1; }
			tmp = temp_map[(iy+1)*TEMP_MAP_W+(ix+1)].wall; while(tmp) { neigh_w_cnt++; tmp>>=1; }
			tmp = temp_map[(iy+1)*TEMP_MAP_W+(ix-1)].wall; while(tmp) { neigh_w_cnt++; tmp>>=1; }
			tmp = temp_map[(iy+1)*TEMP_MAP_W+(ix  )].wall; while(tmp) { neigh_w_cnt++; tmp>>=1; }
			tmp = temp_map[(iy-1)*TEMP_MAP_W+(ix+1)].wall; while(tmp) { neigh_w_cnt++; tmp>>=1; }
			tmp = temp_map[(iy-1)*TEMP_MAP_W+(ix-1)].wall; while(tmp) { neigh_w_cnt++; tmp>>=1; }
			tmp = temp_map[(iy-1)*TEMP_MAP_W+(ix  )].wall; while(tmp) { neigh_w_cnt++; tmp>>=1; }


			if(w_cnt > 3) // A wall is very clearly here.
			{
				int px = pagex, py = pagey;

				int copy_px = px - copy_pagex_start;
				int copy_py = py - copy_pagey_start;

				if(copy_px < 0 || copy_px > 2 || copy_py < 0 || copy_py > 2 || (copy_px == 0 && offsx < 3) || (copy_py == 0 && offsy < 3) ||
				   (copy_px == 2 && offsx > MAP_PAGE_W-4) || (copy_py == 2 && offsy > MAP_PAGE_W-4))
				{
					printf("ERROR: invalid copy_px (%d) and/or copy_py (%d)\n", copy_px, copy_py);
					free(temp_map);
					return -3;
				}

				int found = 0;
				for(int i=0; i<25; i++)
				{
					px = pagex;
					py = pagey;
					int ox = offsx+search_order[i][0];
					int oy = offsy+search_order[i][1];

					if(ox >= MAP_PAGE_W) {ox-=MAP_PAGE_W; px++;}
					else if(ox < 0) {ox+=MAP_PAGE_W; px--;}
					if(oy >= MAP_PAGE_W) {oy-=MAP_PAGE_W; py++;}
					else if(oy < 0) {oy+=MAP_PAGE_W; py--;}

					copy_px = px - copy_pagex_start;
					copy_py = py - copy_pagey_start;

					if((copies[copy_px][copy_py].units[ox][oy].num_obstacles))
					{
						if(!spot_used[copy_px][copy_py][ox][oy])
						{
							avg_drift_cnt++;
							avg_drift_x += search_order[i][0];
							avg_drift_y += search_order[i][1];

							// Existing wall here, it suffices, increase the seen count.
							PLUS_SAT_255(w->pages[px][py]->units[ox][oy].num_seen);
							PLUS_SAT_255(w->pages[px][py]->units[ox][oy].num_obstacles);

							if(w->pages[pagex][pagey]->units[offsx][offsy].num_obstacles > 2)
								w->pages[pagex][pagey]->units[offsx][offsy].result |= UNIT_WALL;

							spot_used[copy_px][copy_py][ox][oy] = 1;
							w->changed[px][py] = 1;
							found = 1;
							break;
						}
					}
				}

				if(!found)
				{
					// We have a new wall.
					w->pages[pagex][pagey]->units[offsx][offsy].result |= UNIT_MAPPED;
					PLUS_SAT_255(w->pages[pagex][pagey]->units[offsx][offsy].num_seen);
					PLUS_SAT_255(w->pages[pagex][pagey]->units[offsx][offsy].num_obstacles);
					w->changed[pagex][pagey] = 1;
				}
			}

			if(w_cnt == 0 && s_cnt > 3)
			{
				// We don't have a wall, but we mapped this unit nevertheless.
				w->pages[pagex][pagey]->units[offsx][offsy].result |= UNIT_MAPPED;
				PLUS_SAT_255(w->pages[pagex][pagey]->units[offsx][offsy].num_seen);
				MINUS_SAT_0(w->pages[pagex][pagey]->units[offsx][offsy].num_obstacles);

				if(
				   ( s_cnt > 5 && neigh_w_cnt == 0 && // we are quite sure:
				   ((int)w->pages[pagex][pagey]->units[offsx][offsy].num_seen > (2*(int)w->pages[pagex][pagey]->units[offsx][offsy].num_obstacles + 3)))
				   || (neigh_w_cnt < 2 &&  // there is 1 wall neighbor, so we are not so sure, but do it eventually.
				   ((int)w->pages[pagex][pagey]->units[offsx][offsy].num_seen > (5*(int)w->pages[pagex][pagey]->units[offsx][offsy].num_obstacles + 10))))
				{
					// Wall has vanished
					w->pages[pagex][pagey]->units[offsx][offsy].result &= ~(UNIT_WALL);
				}
				w->changed[pagex][pagey] = 1;
			}
		}
	}

	if(avg_drift_cnt > 50)
	{
		*after_dx = (avg_drift_x*MAP_UNIT_W)/avg_drift_cnt;
		*after_dy = (avg_drift_y*MAP_UNIT_W)/avg_drift_cnt;
	}

	printf("INFO: Average adjustment during map insertion: x=%d mm, y=%d mm (%d samples)\n", *after_dx, *after_dy, avg_drift_cnt);


	free(temp_map);
	return 0;
}

void lidars_avg_midpoint(int n_lidars, lidar_scan_t** lidar_list, int32_t* mid_x, int32_t* mid_y)
{
	int64_t x = 0;
	int64_t y = 0;
	for(int l=0; l<n_lidars; l++)
	{
		lidar_scan_t* lid = lidar_list[l];
		x += lid->robot_pos.x;
		y += lid->robot_pos.y;
	}

	*mid_x = x / n_lidars;
	*mid_y = y / n_lidars;
}

/*
map_lidars takes a set of lidar scans, assumes they are in sync (i.e., robot coordinates relative
between the images are correct enough), searches for the map around expected coordinates to find
the location where the scans fit the best. Last, the map is modified with the new data.

This function is supposed to be used with a set of lidar scans that complement each other's blind
areas, and to some extent, expand the lidar range (in moderation). For example, the combined image
from 10 scans, that ranges a 5 x 8 meter area with 2000 points, instead of a single 5x5 meter image with 200 points,
would do great!

With single images, mapping is a bit uncertain, because if the image has rather few points, it has high
chances of accidentally matching another similar-looking area; even within the smallish search range.

On the other hand, using too many images covering a large area, or images that have high risk of being out of sync,
will result in a seriously messed up map, since the scans are assumed to be "in sync", and only
minor (one 40mm unit) adjustments will be made within the combined image.

For the same reason, you should use some mechanisms to choose images that are going to be in sync.

Note that this function also assumes that the scans have fairly well matching coordinates already, so this
does not implement "I'm lost, where am I?" functionality.

*/

int bigger_search_area = 0;

void map_next_with_larger_search_area()
{
	if(bigger_search_area == 0)
		bigger_search_area = 1;
}


int map_lidars(world_t* w, int n_lidars, lidar_scan_t** lidar_list, int* da, int* dx, int* dy)
{
	*da = 0;
	*dx = 0;
	*dy = 0;

	if(n_lidars > 32)
	{
		printf("Error: n_lidars must be <=32\n");
		return 1;
	}

	FILE* fdbg = fopen("map_verbose.csv", "w");
	if(!fdbg)
	{
		printf("Error: couldn't open map_verbose.csv for write.\n");
		return 1;
	}

	printf("Info: Attempting to map %d lidar images\n", n_lidars);

	prefilter_lidar_list(n_lidars, lidar_list);


	int mid_x, mid_y;

	// Calculate average robot coordinates between the images, to find arithmetical midpoint.
	// When correcting angle, image is rotated around this point.
	lidars_avg_midpoint(n_lidars, lidar_list, &mid_x, &mid_y);

	fprintf(fdbg, "PASS 1\nda;dx;dy;score;match_walls;exacts;new_walls;discovered_walls\n");

	int a_range = 3;
	int x_range = 240;
	int y_range = 240;
	int a_step = 1*ANG_1_DEG;

	if(bigger_search_area == 1)
	{
		printf("INFO: Using bigger search area, this will take longer\n");
		a_range = 5;
		x_range = 400;
		y_range = 400;
		a_step = 1*ANG_1_DEG;
	}
	else if(bigger_search_area == 2)
	{
		printf("INFO: Using MUCH bigger search area, this will take quite long\n");
		a_range = 20;
		x_range = 560;
		y_range = 560;
		a_step = 2*ANG_1_DEG;
	}

	int best_score = -999999;
	int best1_da=0, best1_dx=0, best1_dy=0, best_matched=0;
	for(int ida=-1*a_range*ANG_1_DEG; ida<=a_range*ANG_1_DEG; ida+=a_step)
	{
		for(int idx=-1*x_range; idx<=x_range; idx+=80)
		{
			for(int idy=-1*y_range; idy<=y_range; idy+=80)
			{
				int n_matched_walls=0, n_exactly_matched_walls=0, n_new_walls=0, n_discovered_walls=0;
				int score_now = score(w, n_lidars, lidar_list, 
					ida, idx, idy, mid_x, mid_y,
					&n_matched_walls, &n_exactly_matched_walls, &n_new_walls, &n_discovered_walls);

				fprintf(fdbg, "%.2f;%d;%d;%d;%d;%d;%d;%d\n",
					(float)ida/(float)ANG_1_DEG, idx, idy, score_now, n_matched_walls, n_exactly_matched_walls, n_new_walls, n_discovered_walls);

				if(score_now > best_score)
				{
					best_score = score_now;
					best_matched = n_matched_walls;
					best1_da = ida;
					best1_dx = idx;
					best1_dy = idy;
				}
			}
		}
	}

	int best_da = 0;
	int best_dx = 0;
	int best_dy = 0;

	int do_not_map = 0;
	if(best_score < 3000)
	{
		if(best_matched == 0) // zero matched walls
		{
			printf("Info: area seems unmapped, and is being mapped with no correction.\n");
			bigger_search_area = 0;
		}
		else
		{
			printf("Info: best score (%d) is so low that we are clearly lost! Mapping is prevented.\n", best_score);
			do_not_map = 1;
			bigger_search_area = 2;
		}
	}
	else
	{
		bigger_search_area = 0;
		fprintf(fdbg, "\nPASS 2\nda;dx;dy;score;match_walls;exacts;new_walls;discovered_walls\n");

		best_score = -999999;
		int best2_da=0, best2_dx=0, best2_dy=0;
		for(int ida=best1_da-2*ANG_0_5_DEG; ida<=best1_da+2*ANG_0_5_DEG; ida+=ANG_0_5_DEG)
		{
			for(int idx=best1_dx-60; idx<=best1_dx+60; idx+=20)
			{
				for(int idy=best1_dy-60; idy<=best1_dy+60; idy+=20)
				{
					int n_matched_walls=0, n_exactly_matched_walls=0, n_new_walls=0, n_discovered_walls=0;
					int score_now = score(w, n_lidars, lidar_list, 
						ida, idx, idy, mid_x, mid_y,
						&n_matched_walls, &n_exactly_matched_walls, &n_new_walls, &n_discovered_walls);

					fprintf(fdbg, "%.2f;%d;%d;%d;%d;%d;%d;%d\n",
						(float)ida/(float)ANG_1_DEG, idx, idy, score_now, n_matched_walls, n_exactly_matched_walls, n_new_walls, n_discovered_walls);

					if(score_now > best_score)
					{
						best_score = score_now;
						best2_da = ida;
						best2_dx = idx;
						best2_dy = idy;
					}
				}
			}
		}

		best_da = best2_da;
		best_dx = best2_dx;
		best_dy = best2_dy;
	}

	if(!do_not_map)
	{
		printf("Info: Map search complete, correction a=%.1fdeg, x=%dmm, y=%dmm, score=%d\n", (float)best_da/(float)ANG_1_DEG, best_dx, best_dy, best_score);

		int32_t aft_corr_x = 0, aft_corr_y = 0;

		do_mapping(w, n_lidars, lidar_list, best_da, best_dx, best_dy, mid_x, mid_y, &aft_corr_x, &aft_corr_y);

		*da = best_da;
		*dx = best_dx + aft_corr_x;
		*dy = best_dy + aft_corr_y;
	}

	fclose(fdbg);

	return 0;
}


void map_sonar(world_t* w, sonar_scan_t* p_son)
{
	int idx_x, idx_y, offs_x, offs_y;

	// Erase old items, but only if all three sonars show a ping from farther away.
	// Clear items from 500 mm to the nearest ping minus 300 mm, or 2000 mm max

	if(p_son->scan[0].valid && p_son->scan[1].valid && p_son->scan[2].valid)
	{
		float nearest = 2000.0;
		for(int i = 0; i < 3; i++)
		{
			int dx = p_son->scan[i].x - p_son->robot_pos.x;
			int dy = p_son->scan[i].y - p_son->robot_pos.y;

			float cur_len = sqrt(sq(dx) + sq(dy)) - 300.0;

			if(cur_len < nearest) nearest = cur_len;
		}

		if(nearest > 400.0)
		{
			const float step = 3*MAP_UNIT_W;

			float pos = 300.0;
			int terminate = 0;

			int dx = p_son->scan[1].x - p_son->robot_pos.x;
			int dy = p_son->scan[1].y - p_son->robot_pos.y;
			float ang = atan2(dy, dx) + M_PI;
			if(ang < 0.0) ang += 2.0*M_PI;
			else if(ang > 2.0*M_PI) ang -= 2.0*M_PI;

		//	printf("INFO: Clearing items start (%d, %d) ang = %.1f deg, len = %.1f\n", p_son->scan[1].x, p_son->scan[1].y, RADTODEG(ang), nearest);
		//	printf("ang = %.4f  dir = %d \n", ang, dir);

			while(1)
			{
				int x = (cos(ang)*pos + (float)p_son->scan[1].x);
				int y = (sin(ang)*pos + (float)p_son->scan[1].y);

				for(int ix=-4*MAP_UNIT_W; ix<=4*MAP_UNIT_W; ix+=MAP_UNIT_W)
				{
					for(int iy=-4*MAP_UNIT_W; iy<=4*MAP_UNIT_W; iy+=MAP_UNIT_W)
					{	
						page_coords(x+ix,y+iy, &idx_x, &idx_y, &offs_x, &offs_y);
						load_9pages(&world, idx_x, idx_y);
						world.pages[idx_x][idx_y]->units[offs_x][offs_y].result &= ~(UNIT_ITEM);
					}
				}

				if(terminate) break;
				pos += step;
				if(pos > nearest)
				{
					pos = nearest;
					terminate = 1;
				}
			}
		}
	}

	for(int i=0; i<3; i++)
	{
		if(!p_son->scan[i].valid) continue;

		for(int s=0; s<25; s++)
		{
			int x = p_son->scan[i].x+search_order[s][0]*MAP_UNIT_W;
			int y = p_son->scan[i].y+search_order[s][1]*MAP_UNIT_W;
			page_coords(x,y, &idx_x, &idx_y, &offs_x, &offs_y);
			load_9pages(&world, idx_x, idx_y);

			if(world.pages[idx_x][idx_y]->units[offs_x][offs_y].result & UNIT_ITEM)
			{
//				printf("INFO: Item already mapped\n");
				goto ALREADY_MAPPED_ITEM;
			}
		}

		int dx = p_son->scan[i].x - p_son->robot_pos.x;
		int dy = p_son->scan[i].y - p_son->robot_pos.y;

		int sqdist = sq(dx) + sq(dy);

		if(sqdist < sq(1500))
		{
			page_coords(p_son->scan[i].x,p_son->scan[i].y, &idx_x, &idx_y, &offs_x, &offs_y);
			world.pages[idx_x][idx_y]->units[offs_x][offs_y].result |= UNIT_ITEM;
//			printf("INFO: Mapping an item\n");
			//world.changed[idx_x][idx_y] = 1;
		}

		ALREADY_MAPPED_ITEM: ;
	}
}

const int robot_xs = 480;
const int robot_ys = 524;
const int lidar_xoffs = 120;
const int lidar_yoffs = 0;

#define ANG32_TO_RAD(x) ((x)/(360.0*(float)ANGLE_1_DEG) * 2.0 * M_PI)

/*

int can_turn(lidar_scan_t* lid, pos_t dest)
{
	float ang = ((uint32_t)(dest.ang - lid->pos.ang))/(360.0*ANGLE_1_DEG) * 2.0 * M_PI;

	if(ang > M_PI) ang-= M_PI;

	// Test in increments:

	int end = ang*360.0/(2.0*M_PI);
	for(int i = 0; i < end; i += 5)
	{
		float cur_ang = (float)i/360.0*2.0*M_PI;
	}



	float ang = ANG32_TO_RAD((uint32_t)lid->pos.ang);
	float dest_ang = ANG32_TO_RAD((uint32_t)dest->pos.ang);

	float ang_diff = dest_ang - ang;
	if(ang_diff < M_PI) // ccw
	{

	}

}


int how_much_forward_before_hit(lidar_scan_t* lid, pos_t dest)
{
	const int side_extra_gap = 100;

	


}

*/
/*
	Takes a straight segment towards the least mapped area, if there is a route in lidar scan.
*/ 

//int auto_map_goto_next_pos(world_t* w, lidar_scan_t* lid)
//{
//	move_to(msg_cr_dest.x, msg_cr_dest.y);
//}
