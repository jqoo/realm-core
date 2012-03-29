#include "ColumnTable.h"
#include "Table.h"


ColumnTable::ColumnTable(size_t ref_specSet, Array* parent, size_t pndx, Allocator& alloc)
: Column(COLUMN_HASREFS, parent, pndx, alloc), m_ref_specSet(ref_specSet) {}

ColumnTable::ColumnTable(size_t ref_column, size_t ref_specSet, Array* parent, size_t pndx, Allocator& alloc)
: Column(ref_column, parent, pndx, alloc), m_ref_specSet(ref_specSet) {}

Table ColumnTable::GetTable(size_t ndx) {
	assert(ndx < Size());

	const size_t ref_columns = GetAsRef(ndx);
	Allocator& alloc = GetAllocator();

	// Get parent info for subtable
	Array* parent = NULL;
	size_t pndx   = 0;
	GetParentInfo(ndx, parent, pndx);

	return Table(alloc, m_ref_specSet, ref_columns, parent, pndx);
}

const Table ColumnTable::GetTable(size_t ndx) const {
	assert(ndx < Size());

	const size_t ref_columns = GetAsRef(ndx);
	Allocator& alloc = GetAllocator();

	// Even though it is const we still need a parent
	// so that the table can know it is attached
	Array* parent = NULL;
	size_t pndx   = 0;
	GetParentInfo(ndx, parent, pndx);

	return Table(alloc, m_ref_specSet, ref_columns, parent, pndx);
}

Table* ColumnTable::GetTablePtr(size_t ndx) {
	assert(ndx < Size());

	const size_t ref_columns = GetAsRef(ndx);
	Allocator& alloc = GetAllocator();

	// Get parent info for subtable
	Array* parent = NULL;
	size_t pndx   = 0;
	GetParentInfo(ndx, parent, pndx);

	// Receiver will own pointer and has to delete it when done
	return new Table(alloc, m_ref_specSet, ref_columns, parent, pndx);
}

size_t ColumnTable::GetTableSize(size_t ndx) const {
	assert(ndx < Size());

	const size_t ref_columns = GetAsRef(ndx);

	if (ref_columns == 0) return 0;
	else {
		Allocator& alloc = GetAllocator();
		const Table table(alloc, m_ref_specSet, ref_columns, NULL, 0);
		return table.GetSize();
	}
}

bool ColumnTable::Add() {
	Insert(Size()); // zero-ref indicates empty table
	return true;
}

void ColumnTable::Insert(size_t ndx) {
	assert(ndx <= Size());
	
	// zero-ref indicates empty table
	Column::Insert(ndx, 0);
}

void ColumnTable::Delete(size_t ndx) {
	assert(ndx < Size());

	const size_t ref_columns = GetAsRef(ndx);

	// Delete sub-tree
	if (ref_columns != 0) {
		Allocator& alloc = GetAllocator();
		Array columns(ref_columns, (Array*)NULL, 0, alloc);
		columns.Destroy();
	}

	Column::Delete(ndx);
}

void ColumnTable::Clear(size_t ndx) {
	assert(ndx < Size());

	const size_t ref_columns = GetAsRef(ndx);
	if (ref_columns == 0) return; // already empty

	// Delete sub-tree
	Allocator& alloc = GetAllocator();
	Array columns(ref_columns, (Array*)NULL, 0, alloc);
	columns.Destroy();

	// Mark as empty table
	Set(ndx, 0);
}

#ifdef _DEBUG

void ColumnTable::Verify() const {
	Column::Verify();
	
	// Verify each sub-table
	const size_t count = Size();
	for (size_t i = 0; i < count; ++i) {
		const size_t tref = Column::GetAsRef(i);
		if (tref == 0) continue;
		
		const Table t = GetTable(i);
		t.Verify();
	}
}

void ColumnTable::LeafToDot(std::ostream& out, const Array& array) const {
	array.ToDot(out);
	
	const size_t count = array.Size();
	
	for (size_t i = 0; i < count; ++i) {
		const size_t tref = array.GetAsRef(i);
		if (tref == 0) continue;
		
		const Table t = GetTable(i);
		t.ToDot(out);
	}
}

#endif //_DEBUG