/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <thread.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <mips/vm.h>
#include<clock.h>
/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12


/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

/**
 * Added by Pratham Malik
 * Redeclaration of global variable for:
 * 1. Storing the total number of pages
 * 2. Coremap entry structure
 * 3. Coremap initialization checking boolean
 * 3. Coremap global lock
 */

int32_t total_systempages;
int32_t coremap_pages;
struct coremap_entry *coremap;
bool coremap_initialized;
struct lock *coremap_lock;

//ENd of Additions by PM
void
vm_bootstrap(void)
{
	paddr_t firstpaddr, lastpaddr;

	//Create the global coremap lock
	coremap_lock = lock_create("coremap_lock");

	//Get the first and last physical address of RAM
	ram_getsize(&firstpaddr, &lastpaddr);

	//Calculate the total number of pages --
	//TODO ADD COde to Roundoff the result or int will itself do the round off
	int total_page_num = (lastpaddr- firstpaddr) / PAGE_SIZE;
	total_systempages = total_page_num;
	/* pages should be a kernel virtual address !!  */
	coremap = (struct coremap_entry*)PADDR_TO_KVADDR(firstpaddr);
	paddr_t freepaddr = firstpaddr + total_page_num * sizeof(struct coremap_entry);

	//Store the number of coremap Pages
//	int noOfcoremapPages= (freepaddr-firstpaddr)/PAGE_SIZE;
	int num_coremapPages= (freepaddr-firstpaddr)/PAGE_SIZE;
	kprintf("\ntotal pages %d \n",total_systempages);
	kprintf("total coremap pages %d \n",num_coremapPages);

	coremap_pages=num_coremapPages;
	/*
	 * Mark the coremap pages as status as fixed i.e. Set to 1
	 * i.e. pages from firstaddr to freeaddr
	 * i.e. pages from 0 to num_coremapPages
	 */

	for(int i=0;i<num_coremapPages;i++)
	{
		coremap[i].ce_paddr= firstpaddr+i*PAGE_SIZE;
		coremap[i].page_status=1;	//Signifying that it is fixed by kernel

	}

	/*
	 * Mark the pages other than coremap with status as free i.e. Set to 0
	 * i.e. pages from firstaddr to freeaddr
	 * i.e. pages from 0 to num_coremapPages
	 */

	for(int i=num_coremapPages;i<total_page_num;i++)
	{
		coremap[i].ce_paddr= firstpaddr+i*PAGE_SIZE;
		coremap[i].page_status=0;	//Signifying that it is free
		coremap[i].chunk_allocated=0;
		coremap[i].as=NULL;
		coremap[i].time= 0;
	}


	/*
	 * Setting the value for coremap_initialized to be 1
	 * so that now alloc_kpages works differently before and after vm_bootstrap
	 */

	coremap_initialized= 1;
//	kprintf("Exiting VM_Bootstrap \n");


}

paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);

	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */

vaddr_t
alloc_kpages(int npages)
{
	paddr_t pa;
	vaddr_t va;

	if(!coremap_initialized){
		pa = getppages(npages);
		if (pa==0) {
			return 0;
		}
		return PADDR_TO_KVADDR(pa);
	}

	else
	{
		//Means that coremap has been initialized and now allocate pages from the coremap

		lock_acquire(coremap_lock);

		if(npages==1)
		{
			//Call function to find the available page
			int index = find_page_available(npages);

			//Check if the page has to be evicted or was it free
			if(coremap[index].page_status==0)
			{
				//means page was free
				va = PADDR_TO_KVADDR(coremap[index].ce_paddr);

				as_zero_region(coremap[index].ce_paddr,npages);

				//Getting time
				time_t seconds;
				uint32_t nanoseconds;
				gettime(&seconds, &nanoseconds);

				coremap[index].chunk_allocated=1;
				coremap[index].as=curthread->t_addrspace;
				coremap[index].page_status=1;
				coremap[index].time= seconds+nanoseconds;
			}
			else
			{
				/**
				 * Meaning that coremap entry at index has to be evicted
				 * Call function to Find the page table entry marked at address and change the page table present bit to 0
				 */

				//Take the page table lock
				lock_acquire(coremap[index].as->lock_page_table);

				change_page_entry(coremap[index].as->page_table,coremap[index].ce_paddr);

				//Release page table lock
				lock_release(coremap[index].as->lock_page_table);

				va = PADDR_TO_KVADDR(coremap[index].ce_paddr);

				as_zero_region(coremap[index].ce_paddr,npages);

				//Getting time
				time_t seconds;
				uint32_t nanoseconds;
				gettime(&seconds, &nanoseconds);

				coremap[index].chunk_allocated=1;
				coremap[index].as=curthread->t_addrspace;
				coremap[index].page_status=1;
				coremap[index].time= seconds+nanoseconds;

			}

		}
		else if(npages >1)
		{
			int index = find_page_available(npages);
			if(index<0)
				pa=0;
			else
			{
				//Meaning pages found to replace
				//As the pages are contiguous -- Iterate over them and change page entries one by one
				for(int i =index;i<index+npages;i++)
				{
					//Take the page table lock
					lock_acquire(coremap[index].as->lock_page_table);

					change_page_entry(coremap[i].as->page_table,coremap[i].ce_paddr);

					//Release page table lock
					lock_release(coremap[index].as->lock_page_table);

					va = PADDR_TO_KVADDR(coremap[index].ce_paddr);

					//Getting time
					time_t seconds;
					uint32_t nanoseconds;
					gettime(&seconds, &nanoseconds);

					coremap[i].page_status=1;
					coremap[i].chunk_allocated=0;
					coremap[i].as=curthread->t_addrspace;
					coremap[index].time= seconds+nanoseconds;
				}

				coremap[index].chunk_allocated=npages;
				as_zero_region(coremap[index].ce_paddr,npages);

			}
		} //End of if checking whether npages more than 1

		//Release the coremap lock
		lock_release(coremap_lock);

		//Return the physical address retrieved from the coremap
		return va;
	}
}




void
free_kpages(vaddr_t addr)
{


	(void)addr;
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{

	//Variable Declaration
	struct addrspace *as;
	vaddr_t stackbase, stacktop;
	paddr_t paddr=0;

	int permissions=0;
	//For calling page alloc
	bool address_found=false;

	//For TLB
	int i;
	uint32_t ehi, elo;
	int spl;
	//End of variable declaration

	switch (faulttype)
	{
		case VM_FAULT_READONLY:
//		 We always create pages read-write, so we can't get this
			panic("dumbvm: got VM_FAULT_READONLY\n");
		case VM_FAULT_READ:
		case VM_FAULT_WRITE:
		break;

		default:
		return EINVAL;
	}

	as = curthread->t_addrspace;
	if (as == NULL)
	{
		/*
		 * No address space set up. This is probably a kernel
		 * fault early in boot. Return EFAULT so as to panic
		 * instead of getting into an infinite faulting loop.
		*/
		return EFAULT;

	}

	//INCLUDE STACK BASE AND TOP CHECKS LATER

//	 Assert that the address space has been set up properly.
	KASSERT(as->heap_start != 0);
	KASSERT(as->heap_end != 0);
//	KASSERT(as->stackbase_top != 0);
//	KASSERT(as->stackbase_base != 0);

	//Check whether addrspace variables are aligned properly
	KASSERT((as->heap_start & PAGE_FRAME) == as->heap_start);
	KASSERT((as->heap_end & PAGE_FRAME) == as->heap_end);

//	KASSERT((as->stackbase_top & PAGE_FRAME) == as->stackbase_top);
//	KASSERT((as->stackbase_base & PAGE_FRAME) == as->stackbase_end);

//	Check the regions start and end by iterating through the regions

	struct addr_regions *head;

	head = as->regions;
	if(as->regions!=NULL)
	{
		//Iterate over the regions
		while(as->regions !=NULL)
		{
			KASSERT(as->regions->va_start != 0);
			KASSERT(as->regions->region_numpages != 0);

			//Check whether the regions are aligned properly
			KASSERT((as->regions->va_start & PAGE_FRAME) == as->regions->va_start);

			//Iterate to the next region
			as->regions=as->regions->next_region;
		}

		//Assign the region back to head
		as->regions = head;
	}

	//End of checking the regions

	//End of checking the addrspace setup

	//Align the fault address
	faultaddress &= PAGE_FRAME;


	stackbase = USERSTACK - VM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;


	/*
	 * Check which region or stack or heap does the fault address lies in
	 * Assign vbase1 and vtop1 based on those findings
	*/

	while(!(address_found))
	{
			//First check within the stack bases
			if(faultaddress >= stackbase && faultaddress < stacktop)
			{

				//Means faultaddress lies in stackpage
				paddr = handle_address(faultaddress,permissions,as);
				if(paddr>0)
				{
					address_found=true;
				}
				else
					return EFAULT;
			}
			else if(faultaddress >= as->heap_start && faultaddress < as->heap_end)
			{
				//meaning lies in the heap region
				paddr = handle_address(faultaddress,permissions,as);
				if(paddr>0)
				{
					address_found=true;
				}
				else
					return EFAULT;

			}
			else
			{
				//Now Iterate over the regions and check whether it exists in one of the region
				while(as->regions !=NULL)
				{
					if(faultaddress >= as->regions->va_start && faultaddress < as->regions->va_end)
					{
						paddr = handle_address(faultaddress,as->regions->set_permissions,as);
						if(paddr>0)
						{
							//mark found as true
							address_found=true;
						}
						else
							return EFAULT;
					}

					//Iterate to the next region
					as->regions=as->regions->next_region;
				}

				//Assign the region back to head
				as->regions = head;
			}//End of else checking if it exists in the region

			if(!address_found)
			{
				//meaning fault address not found in any of the regions
				return EFAULT;
			}


	}//End of while checking whether address found for the fault address


//	 Disable interrupts on this CPU while frobbing the TLB.
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++)
	{
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID)
		{
			continue;
		}

		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
		splx(spl);
		return EFAULT;

	faulttype=0;
		(void) faultaddress;
	return ENOMEM;
}


/**
 * Author; Pratham Malik
 * Function to handle the fault address and assign pages and update the coremap entries
 * and page table entries.
 * Each condition detail is given inside the function
 */

paddr_t
handle_address(vaddr_t faultaddr,int permissions,struct addrspace *as)
{
	paddr_t pa=0;
	/**
	 * First check if the address space is not null and if page table entry exists
	 */

	if(as->page_table!=NULL)
	{
		/**
		 * Meaning entries exist in the page table of addrspace
		 * First check if the fault address entry exists in the page table entries
		 * 1. Take the pagetable lock and iterate over the page map entries
		 * 2. if entry found -- Do
		 * 	  2.1 Check if present bit is 1 -- Then just assign the entry and return the pa mapped to the entry
		 * 	  2.2 If present bit is 0 -- Then take coremap lock and find an index where the entry can be mapped by calling alloc_upages
		 * 	      map the entry at the index and update the coremap index too
		 * 3. Release the coremap lock and page table lock
		 */

		lock_acquire(as->lock_page_table);

		struct page_table_entry *head;
		head= as->page_table;

		//Iterate over the page table entries

		while(as->page_table !=NULL)
		{
			//Check if faultaddress has been mapped till now
			if(as->page_table->va == faultaddr)
			{
				//Mapping exists - Now check if the present bit for the mapping is true or false
				if(as->page_table->present == 1)
				{
					//present bit is true
					pa = as->page_table->pa;

					lock_release(as->lock_page_table);
					return pa;
				}
				else if(as->page_table->present == 0)
				{
					//present bit is false

					lock_acquire(coremap_lock);

					int index = alloc_upages();

					//update the page table entries at that index
					as->page_table->pa = coremap[index].ce_paddr;
					as->page_table->permissions=permissions;
					as->page_table->present=1;
					as->page_table->va=faultaddr;

					//Zero the region
					as_zero_region(coremap[index].ce_paddr,1);

					//Getting time
					time_t seconds;
					uint32_t nanoseconds;
					gettime(&seconds, &nanoseconds);

					//Update the coremap entries
					coremap[index].as=as;
					coremap[index].chunk_allocated=0;
					coremap[index].page_status=2;
					coremap[index].time=seconds+nanoseconds;

					pa = coremap[index].ce_paddr;

					//Re-assign back the head
					as->page_table = head;

					lock_release(as->lock_page_table);
					lock_release(coremap_lock);
					return pa;
					break;
				}
			}

			//Move to next entry in the page table link list
			as->page_table = as->page_table->next;
		} // End of while iterating over the page table entries

		//Re-assign back the head
		as->page_table = head;

		if(pa==0)
		{
			//Meaning no page has been mapped till now for the fault address that is why pa has not been assigned till now
			struct page_table_entry *entry = (struct page_table_entry *) kmalloc(sizeof(struct page_table_entry));

			//Take the coremap lock and find an index to map the entry
			lock_acquire(coremap_lock);

			int index = alloc_upages();

			//update the page table entries at that index
			entry->pa = coremap[index].ce_paddr;
			entry->permissions =permissions;
			entry->present=1;
			entry->va=faultaddr;
			entry->next=head;

			//Zero the region
			as_zero_region(coremap[index].ce_paddr,1);

			//Getting time
			time_t seconds;
			uint32_t nanoseconds;
			gettime(&seconds, &nanoseconds);

			//Update the coremap entries
			coremap[index].as=as;
			coremap[index].chunk_allocated=0;
			coremap[index].page_status=2;
			coremap[index].time=seconds+nanoseconds;

			pa = coremap[index].ce_paddr;

			/*add the entry created to the head of the page table list*/
			as->page_table= entry;

			//Release the coremap lock
			lock_release(coremap_lock);
			//Release the page table lock
			lock_release(as->lock_page_table);

			return pa;

		}



	}//End of IF checking whether the page table entries is not NULL
	else if(as->page_table==NULL)
	{
		/**
		 * Meaning that there are no entries for the page table as of now
		 * In such case -- Allocate space for the page table entry(Take page table lock) and DO
		 * 1. find index where it can be mapped by calling -- alloc_upages (take coremap lock)
		 * 2. Map the pa to va in the page entry and update coremap index too
		 * 3. release pagetable and coremap lock and assign the pa
		 */

		lock_acquire(as->lock_page_table);

		as->page_table = (struct page_table_entry *) kmalloc(sizeof(struct page_table_entry));

		//Take the coremap lock and find an index to map the entry
		lock_acquire(coremap_lock);

		int index = alloc_upages();

		//update the page table entries at that index
		as->page_table->pa = coremap[index].ce_paddr;
		as->page_table->permissions =permissions;
		as->page_table->present=1;
		as->page_table->va=faultaddr;
		as->page_table->next=NULL;

		//Zero the region
		as_zero_region(coremap[index].ce_paddr,1);

		//Getting time
		time_t seconds;
		uint32_t nanoseconds;
		gettime(&seconds, &nanoseconds);

		//Update the coremap entries
		coremap[index].as=as;
		coremap[index].chunk_allocated=0;
		coremap[index].page_status=2;
		coremap[index].time=seconds+nanoseconds;

		pa = coremap[index].ce_paddr;

		//Release the coremap lock
		lock_release(coremap_lock);
		//Release the page table lock
		lock_release(as->lock_page_table);

		return pa;
	}


	return pa;
}

/**
 * Author: Pratham Malik
 *Call alloc for user pages
 *Returns the index of the coremap entry
 *Any function calling alloc_upages should be having the coremap lock
 */

int
alloc_upages()
{
	int counter=0;
	int index=-1;
	bool page_found=false;
	//Acquire the lock and find a free page

	while(!(page_found))
	{
		for(counter=coremap_pages;counter<total_systempages;counter++)
		{
			if(coremap[counter].page_status==0)
			{
				//Means found the page with status as free
				index=counter;
				page_found=true;

				break;
			}
		}

		/**
		 * Means no page found which is free --
		 * then evict any user page (change this during swapping)
		 * FIND To be EVICTED PAGE - Call find_oldest_page to find the oldest page in the coremap
		 */

		index = find_oldest_page();

		/* Now EVICT the page at the index returned from the find_oldest_page by calling FUNCTION: evict_coremap_entry */

		evict_coremap_entry(index);



	}//End of while loop in page_found

	return index;

}

/**
 * Author: Pratham Malik
 * Function to normally evict the coremap entry at a particular index
 * NOTE: Currently we assume that the coremap lock is being held by the caller of this function
 */

void
evict_coremap_entry(int index)
{
	//Take the page table lock
	lock_acquire(coremap[index].as->lock_page_table);

	change_page_entry(coremap[index].as->page_table,coremap[index].ce_paddr);

	//Release page table lock
	lock_release(coremap[index].as->lock_page_table);

	//Reset all the coremap entries at that index
	coremap[index].as=NULL;
	coremap[index].chunk_allocated=0;
	coremap[index].page_status=0;
	coremap[index].time=0;



}


/**
 * Author: Pratham Malik
 * Function to find and return the index in the coremap
 */
int
find_page_available(int npages)
{
	int return_index=-1;
	int counter=0;
	bool found_status=false;

	//Check whether npages is 1 or more
	if(npages==1)
	{
		while(!(found_status))
		{
			//First check for free pages
			for(counter=coremap_pages;counter<total_systempages;counter++)
			{
				if(coremap[counter].page_status==0)
				{
					//Page is free
					return_index=counter;
					found_status=true;
					break;
				}
			} //End of for loop for checking for free page

			if(!(found_status))
			{
				//Meaning no page found till now
				//Call function to find the earliest non-kernel page and return its index
				return_index = find_oldest_page();
				found_status=true;

			}
		}

	}//End of IF checking whether number of pages requested is one or more
	else if(npages >1)
	{
		/**
		 * Means the number of pages requested is more than 1
		 * As per current understanding this can be called only through kernel
		 */
		return_index = find_npages(npages);

	}//End of else if checking whether the number of pages requested is more than 1

	return return_index;
}

/**
 * Author: Pratham Malik
 * The function find_oldest_page checks oldest iterating over the coremap entries
 * It checks whether the page:
 *  1. Is not a kernel page
 *  2. Is the oldest as per timestamp
 *  3. The current addrspace pointer is not equal to the address structure variable
 */
int
find_oldest_page()
{
	int counter=0;

	//Initialize the time and index as the entry for the first system page
	int index=coremap_pages;
	int32_t time=coremap[index].time;
	//Run algorithm to over coremap entries
	for(counter=coremap_pages+1;counter<total_systempages;counter++)
	{
		if( (coremap[counter].page_status !=1) && (coremap[counter].as != curthread->t_addrspace) )
		{
			//Means page status is not fixed and address space is not same as current thread address space
			if(time>coremap[counter].time)
			{
				//Means coremap entry time is less than the earlier time
				time=coremap[counter].time;
				index= counter;
			}
		}
	}

	return index;
}

/**
 * Author: Pratham Malik
 * The function find_npages finds the continuous set of pages iterating over the coremap entries
 * It checks whether the page:
 *  1. Is not a kernel page
 */

int
find_npages(int npages)
{
	int32_t startindex=-1;
	bool found_range= false;
	int32_t count=0;

	//While code for checking it it is free
	while(!found_range)
	{
		for(int i=startindex+1;i<total_systempages;i++)
		{
			if(coremap[i].page_status==0)
			{
				//Meaning that page at index 'i' is free
				//CHeck till npages if it is free
				for(int j=i;j< i+ npages;j++)
				{
					if(coremap[j].page_status == 0)
					{
						count++;
					}
					else
						break;
				}//End of for looping from i till npages

				//Check if count is equal to npages
				if(count==npages)
				{
					//Means contiguous n pages found
					startindex=i;
					found_range=true;
				}
				else
				{
					count=0;

				}

			}// End of if checking whether page is free
		}// End of for loop over all the pages
	}//end of main while

	startindex=-1;
	found_range= false;
	count=0;

	while(!found_range)
	{
		for(int i=startindex+1;i<total_systempages;i++)
		{
			if(coremap[i].page_status==0 || coremap[i].page_status!=1)
			{
				//Meaning that page at index 'i' is free
				//CHeck till npages if it is free
				for(int j=i;j< i+ npages;j++)
				{
					if(coremap[j].page_status == 0 || coremap[j].page_status!=1)
					{
						count++;
					}
					else
						break;
				}//End of for looping from i till npages

				//Check if count is equal to npages
				if(count==npages)
				{
					//Means contiguous n pages found
					startindex=i;
					found_range=true;
				}
				else
				{
					count=0;

				}
			}// End of if checking whether page is free
		}// End of for loop over all the pages
	}//end of main while


	return startindex;
}



/**
 * Author: Pratham Malik
 * The function change_page_entry iterates over page table entries
 * and changes the present bit
 *
 */

void
change_page_entry(struct page_table_entry *entry,paddr_t pa)
{
	struct page_table_entry *head;

	//Means function called to make present bit flag as zero -- that means entry evicted
	//Check if the entry is not null
	if(entry!=NULL)
	{
		head= entry;
		//Iterate over the page table entries

		while(entry !=NULL)
		{
			if(pa == entry->pa)
			{
				//Entry found where pa has been mapped
				entry->present=0;
			}

			entry = entry->next;

		}
			entry = head;
	}
}
