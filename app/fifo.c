#include "app_config.h"
#include "fifo.h"
#include "hardware/sync.h"

static struct
{
	struct fifo_item fifo[KEY_FIFO_SIZE];
	uint8_t count;
	uint8_t read_idx;
	uint8_t write_idx;
} self;

uint8_t fifo_count(void)
{
	uint32_t interrupts = save_and_disable_interrupts();
	uint8_t count = self.count;
	restore_interrupts(interrupts);
	return count;
}

void fifo_flush(void)
{
	uint32_t interrupts = save_and_disable_interrupts();
	self.write_idx = 0;
	self.read_idx = 0;
	self.count = 0;
	restore_interrupts(interrupts);
}

bool fifo_enqueue(const struct fifo_item item)
{
	uint32_t interrupts = save_and_disable_interrupts();
	
	if (self.count >= KEY_FIFO_SIZE) {
		restore_interrupts(interrupts);
		return false;
	}

	self.fifo[self.write_idx++] = item;
	self.write_idx %= KEY_FIFO_SIZE;
	++self.count;

	restore_interrupts(interrupts);
	return true;
}

void fifo_enqueue_force(const struct fifo_item item)
{
	uint32_t interrupts = save_and_disable_interrupts();
	
	if (self.count < KEY_FIFO_SIZE) {
		self.fifo[self.write_idx++] = item;
		self.write_idx %= KEY_FIFO_SIZE;
		++self.count;
	} else {
		self.fifo[self.write_idx++] = item;
		self.write_idx %= KEY_FIFO_SIZE;
		self.read_idx++;
		self.read_idx %= KEY_FIFO_SIZE;
	}
	
	restore_interrupts(interrupts);
}

struct fifo_item fifo_dequeue(void)
{
	uint32_t interrupts = save_and_disable_interrupts();
	
	struct fifo_item item = { 0 };
	if (self.count == 0) {
		restore_interrupts(interrupts);
		return item;
	}

	item = self.fifo[self.read_idx++];
	self.read_idx %= KEY_FIFO_SIZE;
	--self.count;

	restore_interrupts(interrupts);
	return item;
}
