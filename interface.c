/*
 tg
 Copyright (C) 2015 Marcello Mamino
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License version 2 as
 published by the Free Software Foundation.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License along
 with this program; if not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "tg.h"
#include <stdarg.h>
#include <gtk/gtk.h>

#include <cairo-pdf.h>
#include <libgen.h>
#include <string.h>

int preset_bph[] = PRESET_BPH;

cairo_pattern_t *bg_color,*waveform_color,*grid2_color,*grid_color,*pulse_color,*range_color,*stopped_color,*icon1,*icon2,*text_color,*tick_color,*tock_color;

void print_debug(char *format,...)
{
	va_list args;
	va_start(args,format);
	vfprintf(stderr,format,args);
	va_end(args);
}

void error(char *format,...)
{
	char s[100];
	va_list args;
	
	va_start(args,format);
	int size = vsnprintf(s,100,format,args);
	va_end(args);
	
	char *t;
	if (size < 100) {
		t = s;
	} else {
		t = alloca(size+1);
		va_start(args,format);
		vsnprintf(t,size+1,format,args);
		va_end(args);
	}
	
	fprintf(stderr,"%s\n",t);
	
	GtkWidget *dialog = gtk_message_dialog_new(NULL,0,GTK_MESSAGE_ERROR,GTK_BUTTONS_CLOSE,"%s",t);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}

/* Convert a GTK color object into a Cairo object for drawing */
void define_color(cairo_pattern_t **gc, GdkRGBA col)
{
	*gc = cairo_pattern_create_rgb(col.red, col.green, col.blue);
}

/* Grab color definitions from css */
void initialize_palette(GtkWidget *win)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(win);
	
	GdkRGBA color;
	
	gtk_style_context_lookup_color(sc, "graph_background", &color);
	define_color(&bg_color, color);
	gtk_style_context_lookup_color(sc, "waveform_active", &color);
	define_color(&waveform_color, color);
	gtk_style_context_lookup_color(sc, "waveform_stopped", &color);
	define_color(&stopped_color, color);
	gtk_style_context_lookup_color(sc, "grid_line", &color);
	define_color(&grid_color, color);
	gtk_style_context_lookup_color(sc, "grid_line_alternate", &color);
	define_color(&grid2_color, color);
	gtk_style_context_lookup_color(sc, "pulse", &color);
	define_color(&pulse_color, color);
	gtk_style_context_lookup_color(sc, "pulse_range", &color);
	define_color(&range_color, color);
	
	gtk_style_context_lookup_color(sc, "text", &color);
	define_color(&text_color, color);
	gtk_style_context_lookup_color(sc, "icon_on", &color);
	define_color(&icon1, color);
	gtk_style_context_lookup_color(sc, "icon_off", &color);
	define_color(&icon2, color);
	gtk_style_context_lookup_color(sc, "tick", &color);
	define_color(&tick_color, color);
	gtk_style_context_lookup_color(sc, "tock", &color);
	define_color(&tock_color, color);
}

struct main_window {
	GtkWidget *window;
	GtkWidget *settings_grid;
	GtkWidget *bph_combo_box;
	GtkWidget *la_spin_button;
	GtkWidget *icon_drawing_area;
	GtkWidget *rate_label;
	GtkWidget *beaterror_label;
	GtkWidget *amplitude_label;
	GtkWidget *bph_label;
	GtkWidget *panes;
	GtkWidget *tic_drawing_area;
	GtkWidget *toc_drawing_area;
	GtkWidget *period_drawing_area;
	GtkWidget *paperstrip_drawing_area;
	GtkWidget *clear_button;
	GtkWidget *center_button;
#ifdef DEBUG
	GtkWidget *fps_label;
	GtkWidget *debug_drawing_area;
#endif
	
	struct processing_buffers *bfs;
	struct processing_buffers *old;
	
	int bph; // User selected bph. 0 if "Automatic"
	int guessed_bph; // Calculated bph
	int last_bph;
	double la;
	double sample_rate;
	
	uint64_t *events; // List of ticks and tocks
	int events_wp;
	uint64_t events_from;
	double trace_centering;
	int trace_zoom;
	
	int signal;
	struct Settings conf;
};

/* Redraw the DrawingArea widgets (waveforms etc.) */
void redraw(struct main_window *w)
{
	gtk_widget_queue_draw(w->icon_drawing_area);
	gtk_widget_queue_draw(w->tic_drawing_area);
	gtk_widget_queue_draw(w->toc_drawing_area);
	gtk_widget_queue_draw(w->period_drawing_area);
	gtk_widget_queue_draw(w->paperstrip_drawing_area);
#ifdef DEBUG
	gtk_widget_queue_draw(w->debug_drawing_area);
#endif
}

/* Find the preset bph value closest corresponding to the current period */
int guess_bph(double period)
{
	double bph = 7200 / period;
	double min = bph;
	int i,ret;
	
	ret = 0;
	for (i=0; preset_bph[i]; i++) {
		double diff = fabs(bph - preset_bph[i]);
		if (diff < min) {
			min = diff;
			ret = i;
		}
	}
	
	return preset_bph[ret];
}

/* Get data results and a flag indicating if it's current or old */
struct processing_buffers *get_data(struct main_window *w, int *old)
{
	struct processing_buffers *p = w->bfs;
	int i;
	for (i=0; i<NSTEPS && p[i].ready; i++);
	for (i--; i>=0 && p[i].sigma > p[i].period / 10000; i--);
	if (i >= 0) {
		// TODO: Can't we have one object kept alive instead of destroying/creating new ones every time?
		if (w->old) pb_destroy_clone(w->old); // Remove the previous old data
		w->old = pb_clone(&p[i]); // Store the current data as old
		*old = 0;
		return &p[i];
	} else { // Mark as old
		*old = 1;
		return w->old;
	}
}

void recompute(struct main_window *w)
{
	w->signal = analyze_pa_data(w->bfs, w->bph, w->events_from);
	int old;
	struct processing_buffers *p = get_data(w, &old);
	if (old) w->signal = -w->signal;
	if (p)
		// If we have a bph set, use that for the "guess". Otherwise, calculate a guess.
		w->guessed_bph = w->bph ? w->bph : guess_bph(p->period / w->sample_rate);
}

double get_rate(int bph, double sample_rate, struct processing_buffers *p)
{
	return (7200/(bph*p->period / sample_rate) - 1)*24*3600;
}

/* Calculate the amplitude from the lift angle and audio signal */
double get_amplitude(double la, struct processing_buffers *p)
{
	double ret = -1;
	if (p->tic_pulse > 0 && p->toc_pulse > 0) {
		double tic_amp = la * .5 / sin(M_PI * p->tic_pulse / p->period);
		double toc_amp = la * .5 / sin(M_PI * p->toc_pulse / p->period);
		if (la < tic_amp && tic_amp < 360 && la < toc_amp && toc_amp < 360 && fabs(tic_amp - toc_amp) < 60)
			ret = (tic_amp + toc_amp) / 2;
	}
	return ret;
}

void set_rate_label(struct main_window *w, int rate)
{
	char output[6];
	snprintf(output, 6, "%+4d", rate);
	gtk_label_set_text(GTK_LABEL(w->rate_label), output);
}

void set_beaterror_label(struct main_window *w, double be)
{
	char output[8];
	snprintf(output, 8, "%4.1f", be);
	gtk_label_set_text(GTK_LABEL(w->beaterror_label), output);
	// gtk_widget_set_tooltip_text(w->beaterror_label, "12˚"); // Alternate unit display
}

void set_amplitude_label(struct main_window *w, double amp)
{
	char output[8];
	snprintf(output, 8, "%3.0f˚", amp);
	gtk_label_set_text(GTK_LABEL(w->amplitude_label), output);
}

void set_bph_label(struct main_window *w, int bph)
{
	char output[8];
	snprintf(output, 8, "%d", bph);
	gtk_label_set_text(GTK_LABEL(w->bph_label), output);
}

#ifdef DEBUG
void set_fps_label(struct main_window *w, double fps)
{
	char output[12];
	snprintf(output, 12, "%.2f fps", fps);
	gtk_label_set_text(GTK_LABEL(w->fps_label), output);
}
#endif

/* Called 10 times/second to keep the UI updated */
guint refresh_window(struct main_window *w)
{
	recompute(w);
	
	int old;
	struct processing_buffers *p = get_data(w, &old);
	
	if (p) {
		// Update the info labels
		int bph = w->guessed_bph;
		set_bph_label(w, bph);
		int rate = round(get_rate(bph, w->sample_rate, p));
		set_rate_label(w, rate);
		double be = fabs(p->be) * 1000 / p->sample_rate;
		set_beaterror_label(w, be);
		double amp = get_amplitude(w->la, p);
		set_amplitude_label(w, amp);
	} else {
		// Set labels to "---" or leave with last data?
	}
	
#ifdef DEBUG
	// FPS display
	{
		static GTimer *timer = NULL;
		if (!timer) timer = g_timer_new();
		else {
			double fps = 1./g_timer_elapsed(timer, NULL);
			set_fps_label(w, fps);
			g_timer_reset(timer);
		}
	}
#endif
	
	redraw(w); // Redraw the DrawingArea widgets (waveforms etc.)
	
	return TRUE;
}

/* Draw the audio waveform */
void draw_graph(double a, double b, cairo_t *cr, struct processing_buffers *p, GtkWidget *da)
{
	int width = gtk_widget_get_allocated_width(da);
	int height = gtk_widget_get_allocated_height(da);
	
	int n;
	
	int first = TRUE;
	for (n=0; n<2*width; n++) {
		int i = n < width ? n : 2*width - 1 - n;
		double x = fmod(a + i * (b-a) / width, p->period);
		if (x < 0) x += p->period;
		int j = floor(x);
		double y;
		
		if (p->waveform[j] <= 0) y = 0;
		else y = p->waveform[j] * 0.4 / p->waveform_max;
		
		int k = round(y*height);
		if (n < width) k = -k;
		
		if (first) {
			cairo_move_to(cr, i+.5, height/2+k+.5);
			first = FALSE;
		} else
			cairo_line_to(cr, i+.5, height/2+k+.5);
	}
}

#ifdef DEBUG
void draw_debug_graph(double a, double b, cairo_t *c, struct processing_buffers *p, GtkWidget *da)
{
	int width = gtk_widget_get_allocated_width(da);
	int height = gtk_widget_get_allocated_height(da);
	
	int i;
	float max = 0;
	
	int ai = round(a);
	int bi = 1+round(b);
	if (ai < 0) ai = 0;
	if (bi > p->sample_count) bi = p->sample_count;
	for (i=ai; i<bi; i++)
		if (p->debug[i] > max)
			max = p->debug[i];
	
	int first = 1;
	for (i=0; i<width; i++) {
		if ( round(a + i*(b-a)/width) != round(a + (i+1)*(b-a)/width) ) {
			int j = round(a + i*(b-a)/width);
			if (j < 0) j = 0;
			if (j >= p->sample_count) j = p->sample_count-1;
			
			int k = round((0.1+p->debug[j]/max)*0.8*height);
			
			if (first) {
				cairo_move_to(c,i+.5,height-k-.5);
				first = 0;
			} else
				cairo_line_to(c,i+.5,height-k-.5);
		}
	}
}
#endif

double amplitude_to_time(double lift_angle, double amp)
{
	return asin(lift_angle / (2 * amp)) / M_PI;
}

/* Set up default line width and background color before each drawing operation */
void cairo_init(cairo_t *cr)
{
	cairo_set_line_width(cr, 1);
	
	cairo_set_source(cr, bg_color);
	cairo_paint(cr);
}

/* Draws either the tic or toc waveform & grid in their respective widget */
void draw_waveform(
				   struct main_window *w,
				   GtkWidget *da,
				   cairo_t *cr,
				   int (*get_offset)(struct processing_buffers*),
				   double (*get_pulse)(struct processing_buffers*) )
{
	cairo_init(cr);
	
	int width = gtk_widget_get_allocated_width(da);
	int height = gtk_widget_get_allocated_height(da);
	
	// Calculate font size for the amplitude and timing text on the grid
	int fontsize = gtk_widget_get_allocated_width(w->window) / 90; // TODO: Better sizing. Just keep it at 12?
	if (fontsize < 12)
		fontsize = 12;
	cairo_set_font_size(cr, fontsize);
	
	int i;
	
	// Draw vertical time lines every ms
	for (i = 1-NEGATIVE_SPAN; i < POSITIVE_SPAN; i++) {
		int x = (NEGATIVE_SPAN + i) * width / (POSITIVE_SPAN + NEGATIVE_SPAN);
		cairo_move_to(cr, x + .5, height / 2 + .5);
		cairo_line_to(cr, x + .5, height - .5);
		if (i%5)
			cairo_set_source(cr, grid_color);
		else
			cairo_set_source(cr, grid2_color);
		cairo_stroke(cr);
	}
	
	// Draw numbers for time scale, every 5 ms
	cairo_set_source(cr, text_color);
	for (i = 1-NEGATIVE_SPAN; i < POSITIVE_SPAN; i++) {
		if (!(i%5)) {
			int x = (NEGATIVE_SPAN + i) * width / (POSITIVE_SPAN + NEGATIVE_SPAN);
			char s[10];
			snprintf(s, 10, "%d", i);
			cairo_move_to(cr, x+fontsize/4, height-fontsize/2);
			cairo_show_text(cr, s);
		}
	}
	
	cairo_text_extents_t extents;
	
	// Draw "ms" label
	cairo_text_extents(cr, "ms", &extents);
	cairo_move_to(cr, width - extents.x_advance - fontsize/4, height-fontsize/2);
	cairo_show_text(cr, "ms");
	
	int old; // Flag set if the data isn't current
	struct processing_buffers *p = get_data(w, &old);
	double period = p ? p->period / w->sample_rate : 7200. / w->guessed_bph;
	
	// Draw vertical amplitude lines
	for (i = 10; i < 360; i+=10) {
		if (2*i < w->la) continue;
		double t = period*amplitude_to_time(w->la, i);
		if (t > .001 * NEGATIVE_SPAN) continue;
		int x = round(width * (NEGATIVE_SPAN - 1000*t) / (NEGATIVE_SPAN + POSITIVE_SPAN));
		cairo_move_to(cr, x+.5, .5);
		cairo_line_to(cr, x+.5, height / 2 + .5);
		if (i % 50)
			cairo_set_source(cr, grid_color);
		else
			cairo_set_source(cr, grid2_color);
		cairo_stroke(cr);
	}
	
	// Draw numbers for amplitude scale
	double last_x = 0;
	cairo_set_source(cr, text_color);
	for (i = 50; i < 360; i+=50) {
		double t = period*amplitude_to_time(w->la, i);
		if (t > .001 * NEGATIVE_SPAN) continue;
		int x = round(width * (NEGATIVE_SPAN - 1000*t) / (NEGATIVE_SPAN + POSITIVE_SPAN));
		if (x > last_x) {
			char s[10];
			snprintf(s, 10, "%d", abs(i));
			cairo_move_to(cr, x + fontsize/4, fontsize * 3 / 2);
			cairo_show_text(cr, s);
			cairo_text_extents(cr, s, &extents);
			last_x = x + fontsize/4 + extents.x_advance;
		}
	}
	
	// Draw "deg" label
	cairo_text_extents(cr, "deg", &extents);
	cairo_move_to(cr,width - extents.x_advance - fontsize/4, fontsize * 3 / 2);
	cairo_show_text(cr,"deg");
	
	// Draw audio waveform
	if (p) {
		double span = 0.001 * w->sample_rate;
		int offset = get_offset(p);
		
		double a = offset - span * NEGATIVE_SPAN;
		double b = offset + span * POSITIVE_SPAN;
		
		draw_graph(a,b,cr,p,da);
		
		// Make the audio waveform yellow if it's not current.
		cairo_set_source(cr, old ? stopped_color : waveform_color);
		cairo_stroke_preserve(cr);
		cairo_fill(cr);
		
		double pulse = get_pulse(p);
		if (pulse > 0) {
			// Draw vertical blue line at start of pulse
			int x = round((NEGATIVE_SPAN - pulse * 1000 / p->sample_rate) * width / (POSITIVE_SPAN + NEGATIVE_SPAN));
			cairo_move_to(cr, x, 1);
			cairo_line_to(cr, x, height - 1);
			cairo_set_source(cr, pulse_color);
			cairo_set_line_width(cr, 2);
			cairo_stroke(cr);
		}
	} else { // If no data, just draw the center line
		cairo_move_to(cr, .5, height / 2 + .5);
		cairo_line_to(cr, width - .5, height / 2 + .5);
		cairo_set_source(cr, stopped_color);
		cairo_stroke(cr);
	}
	
}

/* Used as a function parameter in draw_waveform() */
int get_tic(struct processing_buffers *p)
{
	return p->tic;
}

/* Used as a function parameter in draw_waveform() */
int get_toc(struct processing_buffers *p)
{
	return p->toc;
}

/* Used as a function parameter in draw_waveform() */
double get_tic_pulse(struct processing_buffers *p)
{
	return p->tic_pulse;
}

/* Used as a function parameter in draw_waveform() */
double get_toc_pulse(struct processing_buffers *p)
{
	return p->toc_pulse;
}

/* Draw the watch icon in the info area */
gboolean icon_draw_event(GtkWidget *widget, cairo_t *cr, struct main_window *w)
{
	// int width = gtk_widget_get_allocated_width(widget);
	int height = gtk_widget_get_allocated_height(widget);
	
	int happy = !!w->signal;
	
	// Watch hands
	cairo_set_line_width(cr, 5);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); // Rounded hands
	cairo_set_source(cr, happy ? icon1 : icon2);
	cairo_move_to(cr, height * 0.5, height * 0.5);
	cairo_line_to(cr, height * 0.75, height * (0.75 - 0.5*happy));
	cairo_move_to(cr, height * 0.5, height * 0.5);
	cairo_line_to(cr, height * 0.35, height * (0.65 - 0.3*happy));
	cairo_stroke(cr);
	
	// Watch outline
	cairo_set_line_width(cr, 6);
	cairo_arc(cr, height * 0.5, height * 0.5, height * 0.4, 0, 2*M_PI);
	cairo_stroke(cr);
	
	const int l = height * 0.8 / (2*NSTEPS - 1);
	int i;
	cairo_set_line_width(cr, 1);
	for (i = 0; i < w->signal; i++) {
		cairo_move_to(cr, height + 0.5*l, height * 0.9 - 2*i*l);
		cairo_line_to(cr, height + 1.5*l, height * 0.9 - 2*i*l);
		cairo_line_to(cr, height + 1.5*l, height * 0.9 - (2*i+1)*l);
		cairo_line_to(cr, height + 0.5*l, height * 0.9 - (2*i+1)*l);
		cairo_line_to(cr, height + 0.5*l, height * 0.9 - 2*i*l);
		cairo_stroke_preserve(cr);
		cairo_fill(cr);
	}

	return FALSE;
}

gboolean tic_draw_event(GtkWidget *widget, cairo_t *cr, struct main_window *w)
{
	draw_waveform(w, w->tic_drawing_area, cr, get_tic, get_tic_pulse);
	return FALSE;
}

gboolean toc_draw_event(GtkWidget *widget, cairo_t *cr, struct main_window *w)
{
	draw_waveform(w, w->toc_drawing_area, cr, get_toc, get_toc_pulse);
	return FALSE;
}

gboolean period_draw_event(GtkWidget *widget, cairo_t *cr, struct main_window *w)
{
	cairo_init(cr);
	
	int width = gtk_widget_get_allocated_width(widget);
	int height = gtk_widget_get_allocated_height(widget);
	
	int old;
	struct processing_buffers *p = get_data(w, &old);
	
	double toc, a=0, b=0;
	
	if (p) {
		toc = p->tic < p->toc ? p->toc : p->toc + p->period;
		a = ((double)p->tic + toc)/2 - p->period/2;
		b = ((double)p->tic + toc)/2 + p->period/2;
		
		cairo_move_to(cr, (p->tic - a - NEGATIVE_SPAN*.001*w->sample_rate) * width/p->period, 0);
		cairo_line_to(cr, (p->tic - a - NEGATIVE_SPAN*.001*w->sample_rate) * width/p->period, height);
		cairo_line_to(cr, (p->tic - a + POSITIVE_SPAN*.001*w->sample_rate) * width/p->period, height);
		cairo_line_to(cr, (p->tic - a + POSITIVE_SPAN*.001*w->sample_rate) * width/p->period, 0);
		cairo_set_source(cr, range_color);
		cairo_fill(cr);
		
		cairo_move_to(cr, (toc - a - NEGATIVE_SPAN*.001*w->sample_rate) * width/p->period, 0);
		cairo_line_to(cr, (toc - a - NEGATIVE_SPAN*.001*w->sample_rate) * width/p->period, height);
		cairo_line_to(cr, (toc - a + POSITIVE_SPAN*.001*w->sample_rate) * width/p->period, height);
		cairo_line_to(cr, (toc - a + POSITIVE_SPAN*.001*w->sample_rate) * width/p->period, 0);
		cairo_set_source(cr, range_color);
		cairo_fill(cr);
	}
	
	int i;
	for (i = 1; i < 16; i++) {
		int x = i * width / 16;
		cairo_move_to(cr, x+.5, .5);
		cairo_line_to(cr, x+.5, height - .5);
		if (i % 4)
			cairo_set_source(cr, grid_color);
		else
			cairo_set_source(cr, grid2_color);
		cairo_stroke(cr);
	}
	
	if (p) {
		draw_graph(a,b,cr,p,widget);
		
		cairo_set_source(cr, old ? stopped_color : waveform_color);
		cairo_stroke_preserve(cr);
		cairo_fill(cr);
	} else {
		cairo_move_to(cr, .5, height / 2 + .5);
		cairo_line_to(cr, width - .5, height / 2 + .5);
		cairo_set_source(cr, stopped_color);
		cairo_stroke(cr);
	}
	
	return FALSE;
}

extern volatile uint64_t timestamp;

gboolean paperstrip_draw_event(GtkWidget *widget, cairo_t *cr, struct main_window *w)
{
	int i,old;
	struct processing_buffers *p = get_data(w, &old);
#ifdef LIGHT
	uint64_t time = timestamp / 2;
#else
	uint64_t time = timestamp;
#endif
	if (p && !old) {
		uint64_t last = w->events[w->events_wp];
		for (i=0; i<EVENTS_MAX && p->events[i]; i++)
			if (p->events[i] > last + floor(p->period / 4)) {
				if (++w->events_wp == EVENTS_COUNT) w->events_wp = 0;
				w->events[w->events_wp] = p->events[i];
				debug("event at %llu\n", w->events[w->events_wp]);
			}
		w->events_from = p->timestamp - ceil(p->period);
	} else {
		w->events_from = time;
	}
	
	cairo_init(cr);
	
	int width = gtk_widget_get_allocated_width(widget);
	int height = gtk_widget_get_allocated_height(widget);
	
	int stopped = 0;
	if (w->events[w->events_wp] && time > 5 * w->sample_rate + w->events[w->events_wp]) {
		time = 5 * w->sample_rate + w->events[w->events_wp];
		stopped = 1;
	}
	
	int strip_width = round(width / (1 + PAPERSTRIP_MARGIN));
	
	// Draw the 4 directional lines in the paperstrip
	cairo_set_line_width(cr, 1.3);
	
	if (p && w->events[w->events_wp]) {
		double rate = get_rate(w->guessed_bph, w->sample_rate, p);
		double slope = - rate * strip_width * w->trace_zoom / (3600. * 24.);
		if (slope <= 1 && slope >= -1) {
			for (i=0; i<4; i++) {
				double y = 0;
				cairo_move_to(cr, (double)width * (i+.5) / 4, 0);
				for (;;) {
					double x = y * slope + (double)width * (i+.5) / 4;
					x = fmod(x, width);
					if (x < 0) x += width;
					double nx = x + slope * (height - y);
					if (nx >= 0 && nx <= width) {
						cairo_line_to(cr, nx, height);
						break;
					} else {
						double d = slope > 0 ? width - x : x;
						y += d / fabs(slope);
						cairo_line_to(cr, slope > 0 ? width : 0, y);
						y += 1;
						if (y > height) break;
						cairo_move_to(cr, slope > 0 ? 0 : width, y);
					}
				}
			}
			cairo_set_source(cr, pulse_color);
			cairo_stroke(cr);
		}
	}
	
	cairo_set_line_width(cr, 1);
	
	int left_margin = (width - strip_width) / 2;
	int right_margin = (width + strip_width) / 2;
	
	// Draw the 2 vertical lines at the margins of the paperstrip
	cairo_move_to(cr, left_margin + .5, .5);
	cairo_line_to(cr, left_margin + .5, height - .5);
	cairo_move_to(cr, right_margin + .5, .5);
	cairo_line_to(cr, right_margin + .5, height - .5);
	cairo_set_source(cr, grid_color);
	cairo_stroke(cr);
	
	// Draw the horizontal lines of the paperstrip
	double sweep = w->sample_rate * 3600. / w->guessed_bph;
	double now = sweep*ceil(time/sweep);
	double ten_s = w->sample_rate * 10 / sweep;
	double last_line = fmod(now/sweep, ten_s);
	int last_tenth = floor(now/(sweep*ten_s));
	for (i=0;;i++) {
		double y = 0.5 + round(last_line + i*ten_s);
		if (y > height) break;
		cairo_move_to(cr, .5, y);
		cairo_line_to(cr, width-.5, y);
		cairo_set_source(cr, (last_tenth-i)%6 ? grid_color : grid2_color);
		cairo_stroke(cr);
	}
	
	// Plot the tick/tocks on the paperstrip
	for (i = w->events_wp;;) {
		if (!w->events[i]) break;
		double event = now - w->events[i] + w->trace_centering + sweep * PAPERSTRIP_MARGIN / (2 * w->trace_zoom);
		int column = floor(fmod(event, (sweep / w->trace_zoom)) * strip_width / (sweep / w->trace_zoom));

		int row = floor(event / sweep);
		if (row >= height) break;
		cairo_set_source(cr, i%2 ? tick_color : tock_color); // Alternate colors
		cairo_move_to(cr,column,row);
		cairo_line_to(cr,column+1,row);
		cairo_line_to(cr,column+1,row+1);
		cairo_line_to(cr,column,row+1);
		cairo_line_to(cr,column,row);
		// cairo_rectangle (cr, column, row, 1, 1);
		cairo_fill(cr);
		if (column < width - strip_width && row > 0) {
			// Replicate the dot to the right if it's in the margin.
			column += strip_width;
			row -= 1;
			cairo_move_to(cr,column,row);
			cairo_line_to(cr,column+1,row);
			cairo_line_to(cr,column+1,row+1);
			cairo_line_to(cr,column,row+1);
			cairo_line_to(cr,column,row);
			cairo_fill(cr);
		}
		if (--i < 0) i = EVENTS_COUNT - 1;
		if (i == w->events_wp) break;
	}
	
	// Draw the arrowed line for the ms scale at the bottom
	cairo_set_source(cr, text_color);
	cairo_set_line_width(cr, 2);
	cairo_move_to(cr, left_margin + 3, height - 20.5);
	cairo_line_to(cr, right_margin - 3, height - 20.5);
	cairo_stroke(cr);
	cairo_set_line_width(cr,1);
	cairo_move_to(cr, left_margin + .5, height - 20.5);
	cairo_line_to(cr, left_margin + 5.5, height - 15.5);
	cairo_line_to(cr, left_margin + 5.5, height - 25.5);
	cairo_line_to(cr, left_margin + .5, height - 20.5);
	cairo_fill(cr);
	cairo_move_to(cr, right_margin + .5, height - 20.5);
	cairo_line_to(cr, right_margin - 4.5, height - 15.5);
	cairo_line_to(cr, right_margin - 4.5, height - 25.5);
	cairo_line_to(cr, right_margin + .5, height - 20.5);
	cairo_fill(cr);
	
	// Draw the ms scale at the bottom of the paperstrip
	int fontsize = gtk_widget_get_allocated_width(w->window) / 90;
	if (fontsize < 12)
		fontsize = 12;
	cairo_set_font_size(cr, fontsize);
	
	char s[50];
	cairo_text_extents_t extents;
	
	snprintf(s, 50, "%.1f ms", 3600000. / (w->guessed_bph * w->trace_zoom));
	cairo_text_extents(cr,s,&extents);
	cairo_move_to(cr, (width - extents.x_advance)/2, height - 30);
	cairo_show_text(cr, s);
	
	return FALSE;
}

#ifdef DEBUG
gboolean debug_draw_event(GtkWidget *widget, cairo_t *cr, struct main_window *w)
{
	cairo_init(cr);
	
	int old = 0;
	struct processing_buffers *p = get_data(w, &old);
	
	if (p) {
		double a = p->period / 10;
		double b = p->period * 2;
		
		draw_debug_graph(a,b,cr,p,w->debug_drawing_area);
		
		cairo_set_source(cr, old ? stopped_color : waveform_color);
		cairo_stroke(cr);
	}
	
	return FALSE;
}
#endif

/* Called when the user changes the bph box value */
void handle_bph_change(GtkComboBox *b, struct main_window *w)
{
	char *s = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(b));
	if (s) {
		int n;
		char *t;
		n = (int)strtol(s, &t, 10);
		if (*t || n < MIN_BPH || n > MAX_BPH) w->bph = 0;
		else w->bph = w->guessed_bph = n;
		g_free(s);
		recompute(w);
		set_bph_label(w, w->guessed_bph);
		redraw(w);
	}
}

/* Called when the user changes the lift angle value */
void handle_la_change(GtkSpinButton *b, struct main_window *w)
{
	double la = gtk_spin_button_get_value(b);
	if (la < MIN_LA || la > MAX_LA) la = DEFAULT_LA;
	w->la = la;
	if (rintf(la) == la) { // Don't display unnecessary decimals
		char str[8];
		snprintf(str, 8, "%d", (int)la);
		gtk_entry_set_text(GTK_ENTRY(b), str);
	}
	redraw(w);
}

/* Called when the user clicks the Clear button */
void handle_clear_trace(GtkButton *b, struct main_window *w)
{
	memset(w->events, 0, EVENTS_COUNT*sizeof(uint64_t));
	redraw(w);
}

void center_trace(struct main_window *w) {
	uint64_t last_ev = w->events[w->events_wp];
	if (last_ev) {
		double sweep = w->sample_rate * 3600. / (w->trace_zoom * w->guessed_bph);
		w->trace_centering = fmod(last_ev + .5*sweep , sweep);
	} else
		w->trace_centering = 0;
}

/* Called when the user clicks the Center button */
void handle_center_trace(GtkButton *b, struct main_window *w)
{
	center_trace(w);
	gtk_widget_queue_draw(w->paperstrip_drawing_area);
}

gboolean delete_event(GtkWidget *widget, GdkEvent *event, struct main_window *w)
{
	// Save UI size/location settings
	w->conf.window_width = gtk_widget_get_allocated_width(GTK_WIDGET(widget));
	w->conf.window_height = gtk_widget_get_allocated_height(GTK_WIDGET(widget));
	w->conf.pane_pos = gtk_paned_get_position(GTK_PANED(w->panes));
	save_settings(&w->conf);
	
	// Stop timeout
	g_source_remove_by_user_data((gpointer)w);
	
	// If you return FALSE in the "delete-event" signal handler
	// GTK will emit the "destroy" signal.
	return FALSE;
}

void quit(GtkWidget *widget, struct main_window *w)
{
	// (Can't save window sizes here because it's destroyed by now)
	// We save that on the delete signal instead
	gtk_main_quit();
}

/* Called when the user scrolls the mouse wheel over the paperstrip */
gboolean paperstrip_scroll_event(GtkWidget *widget, GdkEvent *event, struct main_window *w)
{
	if (event->scroll.direction == GDK_SCROLL_UP) {
		w->trace_zoom++;
		if (w->trace_zoom >20) w->trace_zoom = 20;
	} else
	if (event->scroll.direction == GDK_SCROLL_DOWN) {
		w->trace_zoom--;
		if (w->trace_zoom <1) w->trace_zoom = 1;
	}
	
	center_trace(w);
	
	return TRUE; // Stop event propagation
}

/* Called when file type is changed in the save screenshot dialog */
void handle_filetype_change(GtkComboBox *format_combo, GtkFileChooser *chooser) {
	// The id is known to be the file extension.
	const char *the_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(format_combo));
	
	// Update filter, to only display appropriate files.
	GtkFileFilter *filter = gtk_file_filter_new();
	char *pattern = g_strdup_printf("*.%s", the_id);
	gtk_file_filter_add_pattern(filter, pattern);
	gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(chooser), filter);
	
	// Update filename extension in the chooser
	gchar *filename = gtk_file_chooser_get_filename(chooser);
	if (filename != NULL) {
		// strdup result from basename as we are going to modify it.
		char *base = g_strdup(basename(filename));
		// Remove extension from string, if there is one.
		if (strrchr(base, '.')) *(strrchr(base, '.')) = '\0';
		// Assemble and set new suggested filename
		char *new_filename = g_strdup_printf("%s.%s", base, the_id);
		gtk_file_chooser_set_current_name(chooser, new_filename);
		
		g_free(filename);
		g_free(base);
		g_free(new_filename);
	}
}

/* Display the save screenshot dialog */
void save_screenshot(GtkButton *button, struct main_window *w) {
	// Hide Clear & Center buttons
	gtk_widget_hide(w->clear_button);
	gtk_widget_hide(w->center_button);
	
	// Set up the file chooser
	GtkWidget *chooser = gtk_file_chooser_dialog_new("Save screenshot",
													 GTK_WINDOW(w->window),
													 GTK_FILE_CHOOSER_ACTION_SAVE,
													 "Cancel", GTK_RESPONSE_CANCEL,
													 "Save", GTK_RESPONSE_ACCEPT,
													 NULL);
	// Generate file name.
	GDateTime *now = g_date_time_new_now_local();
	gchar *d = g_date_time_format(now, "%F at %H.%M.%S");
	g_date_time_unref(now);
	char *filename = g_strdup_printf("%s %s.pdf", g_get_application_name(), d);
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), filename);
	g_free(filename);
	
	gtk_window_set_transient_for(GTK_WINDOW(chooser), GTK_WINDOW(w->window));
	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);
	
	// File type selection combo box
	GtkWidget *format_combo = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "pdf", "Save as PDF (*.pdf)");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "png", "Save as PNG (*.png)");
	g_signal_connect(GTK_COMBO_BOX(format_combo),"changed", G_CALLBACK(handle_filetype_change), chooser);
	
	// Default to PDF
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(format_combo), "pdf");
	
	// Add file type selector in a box as the "extra" of the file chooser dialog
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_hexpand(box, TRUE);
	gtk_widget_set_halign(format_combo, GTK_ALIGN_END);
	gtk_box_pack_start(GTK_BOX(box), format_combo, FALSE, FALSE, 0);
	gtk_widget_show_all(box);
	gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(chooser), box);
	
	// Run the dialog and act if OK was pressed
	int res = gtk_dialog_run(GTK_DIALOG(chooser));
	
	if (res == GTK_RESPONSE_ACCEPT) {
		gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
		
		GtkWidget *widget = w->window; // The source of the image we want to save
		cairo_surface_t *surface;
		
		// Required height to crop the settings area + the borders
		int cropping = gtk_widget_get_allocated_height(w->settings_grid) + gtk_container_get_border_width(GTK_CONTAINER(w->window)) + 3;
		
		int width = gtk_widget_get_allocated_width(widget);
		int height = gtk_widget_get_allocated_height(widget) - cropping;
		
		int type = gtk_combo_box_get_active(GTK_COMBO_BOX(format_combo));
		if (type == 1) { // PNG
			surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
		} else { // PDF
			surface = cairo_pdf_surface_create(filename, 1.0*width, 1.0*height);
		}
		
		cairo_t *cr  = cairo_create(surface);
		cairo_translate(cr, 0.0, cropping*-1.0); // Crop top (settings area)
		gtk_widget_draw(widget, cr);
		cairo_destroy(cr);
		if (type == 1) cairo_surface_write_to_png(surface, filename);
		cairo_surface_destroy(surface);
	}
	
	// Bring back the hidden buttons
	gtk_widget_show(w->clear_button);
	gtk_widget_show(w->center_button);
	// Remove the save dialog
	gtk_widget_destroy(chooser);
}

/* Display the Settings dialog */
void show_preferences(GtkButton *button, struct main_window *w) {
	GtkDialogFlags flags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;
	GtkWidget *dialog = gtk_dialog_new_with_buttons("Settings",
										  GTK_WINDOW(w->window),
										  flags,
										  ("Cancel"), GTK_RESPONSE_CANCEL,
										  "OK", GTK_RESPONSE_ACCEPT,
										  NULL);
	
	// gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE); // Non-resizable
	
	GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_set_border_width(GTK_CONTAINER(content_area), 5);
	GtkWidget *prefs_grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(prefs_grid), 6);
	gtk_grid_set_row_spacing(GTK_GRID(prefs_grid), 4);
	gtk_container_set_border_width(GTK_CONTAINER(prefs_grid), 10);
	gtk_widget_set_halign(prefs_grid, GTK_ALIGN_CENTER); // Keep grid centered in case user resizes window
	gtk_container_add(GTK_CONTAINER(content_area), prefs_grid); // Add the grid to the dialog
	
	GtkWidget *input_label = gtk_label_new("Audio input:");
	gtk_widget_set_tooltip_text(input_label, "What audio input to listen to.");
	gtk_widget_set_halign(input_label, GTK_ALIGN_END); // Right aligned
	gtk_grid_attach(GTK_GRID(prefs_grid), input_label, 0,0,1,1);
	
	GtkWidget *adjust_label = gtk_label_new("Rate adjustment:");
	gtk_widget_set_tooltip_text(adjust_label, "Sound card sampling rate correction.");
	gtk_widget_set_halign(adjust_label, GTK_ALIGN_END); // Right aligned
	gtk_grid_attach(GTK_GRID(prefs_grid), adjust_label, 0,1,1,1);
	
	GtkWidget *cpu_label = gtk_label_new("Precision mode:");
	gtk_widget_set_tooltip_text(cpu_label, "Use more CPU to enhance calculations.");
	gtk_widget_set_halign(cpu_label, GTK_ALIGN_END); // Right aligned
	gtk_grid_attach(GTK_GRID(prefs_grid), cpu_label, 0,2,1,1);
	
	GtkWidget *dark_label = gtk_label_new("Dark UI:");
	gtk_widget_set_tooltip_text(dark_label, "Use a darker user interface with less light pollution.");
	gtk_widget_set_halign(dark_label, GTK_ALIGN_END); // Right aligned
	gtk_grid_attach(GTK_GRID(prefs_grid), dark_label, 0,3,1,1);
	
	GtkWidget *input = gtk_combo_box_text_new();
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(input), "Default");
	int active = 0;
	for (int n=1; n <= num_inputs(); n++) {
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(input), input_name(n));
		if (strcmp(w->conf.audio_input, input_name(n)) == 0) active=n;
	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(input), active); // Fill in value from settings
	gtk_grid_attach_next_to(GTK_GRID(prefs_grid), input, input_label, GTK_POS_RIGHT, 2, 1);
	
	GtkWidget *adjustment = gtk_entry_new();
	gtk_entry_set_input_purpose(GTK_ENTRY(adjustment), GTK_INPUT_PURPOSE_NUMBER);
	gtk_entry_set_alignment(GTK_ENTRY(adjustment), 1); // Right aligned
	gtk_entry_set_max_length(GTK_ENTRY(adjustment), 6);
	gtk_entry_set_width_chars(GTK_ENTRY(adjustment), 6);
	gtk_entry_set_max_width_chars(GTK_ENTRY(adjustment), 6);
	char ra[12];
	snprintf(ra, 12, "%+.2f", w->conf.rate_adjustment);
	gtk_entry_set_text(GTK_ENTRY(adjustment), ra); // Fill in value from settings
	gtk_grid_attach_next_to(GTK_GRID(prefs_grid), adjustment, adjust_label, GTK_POS_RIGHT, 1, 1);
	
	GtkWidget *secs = gtk_label_new("seconds/day");
	gtk_grid_attach_next_to(GTK_GRID(prefs_grid), secs, adjustment, GTK_POS_RIGHT, 1, 1);
	
	GtkWidget *cpu = gtk_check_button_new();
	if (w->conf.precision_mode) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cpu), TRUE);
	gtk_grid_attach_next_to(GTK_GRID(prefs_grid), cpu, cpu_label, GTK_POS_RIGHT, 2, 1);
	
	GtkWidget *dark = gtk_check_button_new();
	if (w->conf.dark_theme) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dark), TRUE);
	gtk_grid_attach_next_to(GTK_GRID(prefs_grid), dark, dark_label, GTK_POS_RIGHT, 2, 1);
	
	gtk_widget_show_all(dialog);
	
	int res = gtk_dialog_run(GTK_DIALOG(dialog)); // Show the dialog
	
	if (res == GTK_RESPONSE_ACCEPT) {
		// Save the dialog data in the settings variable and then save it out to disk
		w->conf.audio_input = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(input));
		w->conf.rate_adjustment = atof(gtk_entry_get_text(GTK_ENTRY(adjustment)));
		w->conf.precision_mode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cpu));
		w->conf.dark_theme = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dark));
		
		save_settings(&w->conf);
	}
	
	// Get rid of the dialog
	gtk_widget_destroy(dialog);
}

/* Set up the main window and populate with widgets */
void init_main_window(struct main_window *w)
{
	w->signal = 0;
	
	w->events = malloc(EVENTS_COUNT * sizeof(uint64_t));
	memset(w->events,0,EVENTS_COUNT * sizeof(uint64_t));
	w->events_wp = 0;
	w->events_from = 0;
	w->trace_centering = 0;
	w->trace_zoom = 10;
	
	w->guessed_bph = w->last_bph = DEFAULT_BPH;
	w->bph = 0;
	w->la = DEFAULT_LA;
	
	gtk_container_set_border_width(GTK_CONTAINER(w->window), 5); // Border around the window
	g_signal_connect(w->window, "delete_event", G_CALLBACK(delete_event), w); // Signal emitted if a user requests that a toplevel window is closed.
	g_signal_connect(w->window, "destroy", G_CALLBACK(quit), w);
	
	gtk_window_set_title(GTK_WINDOW(w->window), PROGRAM_NAME " " VERSION);
	
	// Populate the settings grid
	w->settings_grid = gtk_grid_new(); // The grid containing the settings, default to horizontal orientation
	gtk_grid_set_column_spacing(GTK_GRID(w->settings_grid), 2);
	
	// Beat mode Label
	GtkWidget *bphmode_label = gtk_label_new("Beat mode");
	gtk_container_add(GTK_CONTAINER(w->settings_grid), bphmode_label); // Add to grid
	
	// BPH combo box
	w->bph_combo_box = gtk_combo_box_text_new_with_entry();
	gtk_widget_set_size_request(w->bph_combo_box, 40, -1); // Try making the dropdown less wide. Not working...
	// Fill in pre-defined values
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w->bph_combo_box), "Automatic");
	int *bph;
	for (bph = preset_bph; *bph; bph++) {
		char s[50];
		snprintf(s, 50, "%d", *bph);
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w->bph_combo_box), s);
	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(w->bph_combo_box), 0);
	gtk_widget_set_can_default(w->bph_combo_box, FALSE); // Try to avoid getting the automatic focus. Not working....
	g_signal_connect(w->bph_combo_box, "changed", G_CALLBACK(handle_bph_change), w);
	gtk_container_add(GTK_CONTAINER(w->settings_grid), w->bph_combo_box);
	
	// Lift angle label
	GtkWidget *la_label = gtk_label_new("Lift angle");
	gtk_widget_set_margin_start(la_label, 10); // Make space from the widget in front
	gtk_container_add(GTK_CONTAINER(w->settings_grid), la_label);
	
	// Lift angle spin button
	w->la_spin_button = gtk_spin_button_new_with_range(MIN_LA, MAX_LA, 1);
	gtk_spin_button_set_digits(GTK_SPIN_BUTTON(w->la_spin_button), 2); // Allow for decimal lift angles to be manually entered
	g_signal_connect(w->la_spin_button, "value_changed", G_CALLBACK(handle_la_change), w);
	gtk_container_add(GTK_CONTAINER(w->settings_grid), w->la_spin_button);
	
	// Screenshot button
	GtkWidget *screenshot_button = gtk_button_new_with_label("Screenshot");
	gtk_widget_set_name(screenshot_button, "screenshot_button"); // To allow for CSS styling
	gtk_widget_set_halign(screenshot_button, GTK_ALIGN_END); // Right aligned
	gtk_widget_set_hexpand(screenshot_button, TRUE); // Needed to make the grid expand and make room for the button at the far right
	g_signal_connect(screenshot_button, "clicked", G_CALLBACK(save_screenshot), w);
	gtk_container_add(GTK_CONTAINER(w->settings_grid), screenshot_button);
	
	// Prefs button
	GtkWidget *prefs_button = gtk_button_new_with_label("Settings");
	gtk_widget_set_name(prefs_button, "settings_button"); // To allow for CSS styling
	gtk_widget_set_halign(prefs_button, GTK_ALIGN_END); // Right aligned
	gtk_widget_set_hexpand(prefs_button, FALSE);
	g_signal_connect(prefs_button, "clicked", G_CALLBACK(show_preferences), w);
	gtk_container_add(GTK_CONTAINER(w->settings_grid), prefs_button);
	
	// Info grid
	GtkWidget *info_grid = gtk_grid_new(); // The grid containing the info text, default to horizontal orientation
	gtk_grid_set_column_spacing(GTK_GRID(info_grid), 0);
	
	// Watch icon
	w->icon_drawing_area = gtk_drawing_area_new();
	int width = OUTPUT_WINDOW_HEIGHT + 3*(OUTPUT_WINDOW_HEIGHT * 0.8 / (2*NSTEPS - 1));
	gtk_widget_set_size_request(w->icon_drawing_area, width, OUTPUT_WINDOW_HEIGHT);
	g_signal_connect(w->icon_drawing_area, "draw", G_CALLBACK(icon_draw_event), w);
	gtk_container_add(GTK_CONTAINER(info_grid), w->icon_drawing_area); // Add to grid
	
	// Rate Label
	w->rate_label = gtk_label_new("----");
	gtk_widget_set_valign(w->rate_label, GTK_ALIGN_BASELINE);
	gtk_widget_set_margin_end(w->rate_label, 4);
	gtk_label_set_width_chars(GTK_LABEL(w->rate_label), 4);
	gtk_label_set_xalign(GTK_LABEL(w->rate_label), 1); // Right align
	gtk_widget_set_name(w->rate_label, "rate");
	gtk_container_add(GTK_CONTAINER(info_grid), w->rate_label);
	
	GtkWidget *rate_unit_label = gtk_label_new("s/d");
	gtk_widget_set_valign(rate_unit_label, GTK_ALIGN_BASELINE);
	gtk_widget_set_margin_end(rate_unit_label, 10);
	gtk_label_set_xalign(GTK_LABEL(rate_unit_label), 0); // Left align
	gtk_widget_set_name(rate_unit_label, "rate_units");
	gtk_container_add(GTK_CONTAINER(info_grid), rate_unit_label);
	
	// Beat Error Label
	w->beaterror_label = gtk_label_new("----");
	gtk_widget_set_valign(w->beaterror_label, GTK_ALIGN_BASELINE);
	gtk_widget_set_margin_end(w->beaterror_label, 4);
	gtk_label_set_width_chars(GTK_LABEL(w->beaterror_label), 3);
	gtk_label_set_xalign(GTK_LABEL(w->beaterror_label), 1); // Right align
	gtk_widget_set_name(w->beaterror_label, "beaterror");
	gtk_container_add(GTK_CONTAINER(info_grid), w->beaterror_label);
	
	GtkWidget *beaterror_unit_label = gtk_label_new("ms");
	gtk_widget_set_valign(beaterror_unit_label, GTK_ALIGN_BASELINE);
	gtk_widget_set_margin_end(beaterror_unit_label, 10);
	gtk_label_set_xalign(GTK_LABEL(beaterror_unit_label), 0); // Left align
	gtk_widget_set_name(beaterror_unit_label, "beaterror_units");
	gtk_container_add(GTK_CONTAINER(info_grid), beaterror_unit_label);
	
	// Amplitude Label
	w->amplitude_label = gtk_label_new("---˚");
	gtk_widget_set_valign(w->amplitude_label, GTK_ALIGN_BASELINE);
	gtk_widget_set_margin_end(w->amplitude_label, 10);
	gtk_label_set_width_chars(GTK_LABEL(w->amplitude_label), 4);
	gtk_label_set_xalign(GTK_LABEL(w->amplitude_label), 1); // Right align
	gtk_widget_set_name(w->amplitude_label, "amplitude");
	gtk_container_add(GTK_CONTAINER(info_grid), w->amplitude_label);
	
	// BPH Label
	w->bph_label = gtk_label_new("21600");
	gtk_widget_set_valign(w->bph_label, GTK_ALIGN_BASELINE);
	gtk_widget_set_margin_end(w->bph_label, 4);
	gtk_label_set_width_chars(GTK_LABEL(w->bph_label), 5);
	gtk_label_set_xalign(GTK_LABEL(w->bph_label), 1); // Right align
	gtk_widget_set_name(w->bph_label, "bph");
	gtk_container_add(GTK_CONTAINER(info_grid), w->bph_label);
	
	GtkWidget *bph_unit_label = gtk_label_new("bph");
	gtk_widget_set_valign(bph_unit_label, GTK_ALIGN_BASELINE);
	gtk_widget_set_margin_end(bph_unit_label, 10);
	gtk_label_set_xalign(GTK_LABEL(bph_unit_label), 0); // Left align
	gtk_widget_set_name(bph_unit_label, "bph_units");
	gtk_container_add(GTK_CONTAINER(info_grid), bph_unit_label);
	
	// Populate the panes
	w->panes = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	// gtk_paned_set_wide_handle(GTK_PANED(panes), TRUE); // Requires GTK+ 3.16 but makes the gutter area transparent instead of darker.
	gtk_paned_set_position(GTK_PANED(w->panes), w->conf.pane_pos); // Set the pane divider according to the settings
	GtkWidget *left_grid = gtk_grid_new();
	GtkWidget *right_grid = gtk_grid_new();
	
	// Set direction of the right-hand grid since we're using gtk_container_add() to add contents
	gtk_orientable_set_orientation(GTK_ORIENTABLE(right_grid), GTK_ORIENTATION_VERTICAL);
	
	gtk_grid_set_row_spacing(GTK_GRID(left_grid), 5);
	gtk_grid_set_row_spacing(GTK_GRID(right_grid), 5);
	gtk_grid_set_column_spacing(GTK_GRID(left_grid), 5);
	gtk_grid_set_column_spacing(GTK_GRID(right_grid), 5);
	
	// The 3 waveforms on the right pane should all be equal height
	gtk_grid_set_row_homogeneous(GTK_GRID(right_grid), TRUE);
	
	// Add the grids to the two panes
	gtk_paned_pack1(GTK_PANED(w->panes), left_grid, TRUE, FALSE);
	gtk_paned_pack2(GTK_PANED(w->panes), right_grid, TRUE, FALSE);
	
	gtk_widget_set_size_request(left_grid, 100, -1); // Minimum size of left pane
	gtk_widget_set_size_request(right_grid, 200, 300); // Minimum size of right pane
	
	// Paperstrip
	w->paperstrip_drawing_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(w->paperstrip_drawing_area, 100, 300); // Min width is actually limited by the buttons below (~150px)
	gtk_widget_add_events(w->paperstrip_drawing_area, GDK_SCROLL_MASK); // React to mouse wheel scroll events
	g_signal_connect(w->paperstrip_drawing_area, "draw", G_CALLBACK(paperstrip_draw_event), w);
	g_signal_connect(w->paperstrip_drawing_area, "scroll-event", G_CALLBACK(paperstrip_scroll_event), w);
	gtk_widget_set_hexpand(w->paperstrip_drawing_area, TRUE); // Make sure we expand when pane resizes
	gtk_widget_set_vexpand(w->paperstrip_drawing_area, TRUE);
	gtk_grid_attach(GTK_GRID(left_grid), w->paperstrip_drawing_area, 0,0,2,1);
	
	// CLEAR button
	w->clear_button = gtk_button_new_with_label("Clear");
	gtk_widget_set_name(w->clear_button, "clear_button"); // To allow for CSS styling
	gtk_container_set_border_width(GTK_CONTAINER(w->clear_button), 2);
	g_signal_connect(w->clear_button, "clicked", G_CALLBACK(handle_clear_trace), w);
	gtk_grid_attach(GTK_GRID(left_grid), w->clear_button, 0,1,1,1);
	
	// CENTER button
	w->center_button = gtk_button_new_with_label("Center");
	gtk_widget_set_name(w->center_button, "center_button"); // To allow for CSS styling
	gtk_container_set_border_width(GTK_CONTAINER(w->center_button), 2);
	g_signal_connect(w->center_button, "clicked", G_CALLBACK(handle_center_trace), w);
	gtk_grid_attach(GTK_GRID(left_grid), w->center_button, 1,1,1,1);
	
	// Tic waveform area
	w->tic_drawing_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(w->tic_drawing_area, 400, 100);
	g_signal_connect(w->tic_drawing_area, "draw", G_CALLBACK(tic_draw_event), w);
	gtk_widget_set_hexpand(w->tic_drawing_area, TRUE); // Make sure we expand when pane resizes
	gtk_widget_set_vexpand(w->tic_drawing_area, TRUE);
	gtk_container_add(GTK_CONTAINER(right_grid), w->tic_drawing_area);
	
	// Toc waveform area
	w->toc_drawing_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(w->toc_drawing_area, 400, 100);
	g_signal_connect(w->toc_drawing_area, "draw", G_CALLBACK(toc_draw_event), w);
	gtk_widget_set_hexpand(w->toc_drawing_area, TRUE); // Make sure we expand when pane resizes
	gtk_widget_set_vexpand(w->toc_drawing_area, TRUE);
	gtk_container_add(GTK_CONTAINER(right_grid), w->toc_drawing_area);
	
	// Period waveform area
	w->period_drawing_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(w->period_drawing_area, 400, 100);
	g_signal_connect(w->period_drawing_area, "draw", G_CALLBACK(period_draw_event), w);
	gtk_widget_set_hexpand(w->period_drawing_area, TRUE); // Make sure we expand when pane resizes
	gtk_widget_set_vexpand(w->period_drawing_area, TRUE);
	gtk_container_add(GTK_CONTAINER(right_grid), w->period_drawing_area);
	
#ifdef DEBUG
	w->fps_label = gtk_label_new("0 fps");
	gtk_container_add(GTK_CONTAINER(info_grid), w->fps_label);
	
	w->debug_drawing_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(w->debug_drawing_area, 400, 100);
	g_signal_connect(w->debug_drawing_area, "draw", G_CALLBACK(debug_draw_event), w);
	gtk_container_add(GTK_CONTAINER(right_grid), w->debug_drawing_area);
#endif
	
	// Populate the root grid with the children grids we created above
	GtkWidget *root_grid = gtk_grid_new(); // The grid containing all of the UI
	gtk_orientable_set_orientation(GTK_ORIENTABLE(root_grid), GTK_ORIENTATION_VERTICAL);
	gtk_grid_set_row_spacing(GTK_GRID(root_grid), 5);
	gtk_container_add(GTK_CONTAINER(w->window), root_grid); // Add the root grid to the window
	// Add the child grids inside the root grid
	gtk_container_add(GTK_CONTAINER(root_grid), w->settings_grid);
	gtk_container_add(GTK_CONTAINER(root_grid), info_grid);
	gtk_container_add(GTK_CONTAINER(root_grid), w->panes);
	
	// All done. Show all the widgets.
	gtk_widget_show_all(w->window);
	
	// Set the lift angle spinner to the default value
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->la_spin_button), DEFAULT_LA);
	
	// gtk_window_set_interactive_debugging(TRUE);
}

/* Called when the GTK application starts running */
void activate (GtkApplication* app, gpointer user_data)
{
	// Initialize the "global" w object
	struct main_window w;
	load_settings(&w.conf); // Load app settings

	// Initialize audio
	int nominal_sr;
	double real_sr;
	if (start_portaudio(&nominal_sr, &real_sr, w.conf.audio_input)) return; // Bail out if we can't open audio.
	
	struct processing_buffers p[NSTEPS];
	int i;
	for (i=0; i < NSTEPS; i++) {
		p[i].sample_rate = nominal_sr;
		p[i].sample_count = nominal_sr * (1<<(i+FIRST_STEP));
		setup_buffers(&p[i]);
		p[i].period = -1;
	}
	
	w.sample_rate = real_sr;
	w.bfs = p;
	w.old = NULL;
	w.window = gtk_application_window_new(app);
	
	gtk_window_set_default_size(GTK_WINDOW(w.window), w.conf.window_width, w.conf.window_height);

	if (w.conf.dark_theme) // Use the dark theme
		g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
	
	GtkCssProvider *provider = gtk_css_provider_new();
	GFile *css_file =  g_file_new_for_commandline_arg("tg.css");
	gtk_css_provider_load_from_file(provider, css_file, NULL); // No error handling yet!
	
	GdkDisplay *display = gdk_display_get_default();
	GdkScreen *screen = gdk_display_get_default_screen(display);
	gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	
	g_object_unref(provider);
	g_object_unref(css_file);
	
	initialize_palette(w.window); // Set up the color definitions we'll be using
	
	// Set up GDK+ widgets for the UI
	init_main_window(&w);

	// Call refresh_window() 10 times/second
	g_timeout_add_full(G_PRIORITY_LOW, 100,(GSourceFunc)refresh_window, &w, NULL);
	
	// All GTK applications must have a gtk_main(). Control ends here and waits for an event to occur.
	gtk_main(); // Runs the main loop until gtk_main_quit() is called.
}

/* PROGRAM START */
int main(int argc, char **argv)
{
	GtkApplication *app;
	int status;
	
	app = gtk_application_new("li.ciovil.tg", G_APPLICATION_FLAGS_NONE); // TODO: app id?
	
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	
	return status;
}
