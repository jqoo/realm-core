#define _CRT_SECURE_NO_WARNINGS
#include "Table.hpp"
#include <assert.h>
#include "Index.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include "alloc_slab.hpp"
#include "ColumnStringEnum.hpp"
#include "ColumnTable.hpp"
#include "ColumnMixed.hpp"

using namespace std;

namespace tightdb {

struct FakeParent: Table::Parent {
    virtual void update_child_ref(size_t, size_t) {} // Ignore
    virtual void child_destroyed(size_t) {} // Ignore
    virtual size_t get_child_ref(size_t) const { return 0; }
};


// -- Table ---------------------------------------------------------------------------------

// Create new Table
Table::Table(Allocator& alloc):
    m_size(0),
    m_top(COLUMN_HASREFS, NULL, 0, alloc),
    m_columns(COLUMN_HASREFS, NULL, 0, alloc),
    m_spec_set(alloc, NULL, 0),
    m_ref_count(1)
{
    m_top.Add(m_spec_set.GetRef());
    m_top.Add(m_columns.GetRef());
    m_spec_set.SetParent(&m_top, 0);
    m_columns.SetParent(&m_top, 1);
}

// Create Table from ref
Table::Table(Allocator& alloc, size_t top_ref, Parent* parent, size_t ndx_in_parent):
    m_size(0), m_top(alloc), m_columns(alloc), m_spec_set(alloc), m_ref_count(1)
{
    // Load from allocated memory
    m_top.UpdateRef(top_ref);
    m_top.SetParent(parent, ndx_in_parent);
    assert(m_top.Size() == 2);

    const size_t schema_ref  = m_top.GetAsRef(0);
    const size_t columns_ref = m_top.GetAsRef(1);

    Create(schema_ref, columns_ref, &m_top, 1);
    m_spec_set.SetParent(&m_top, 0);
}

// Create attached table from ref
Table::Table(SubtableTag, Allocator& alloc, size_t top_ref, Parent* parent, size_t ndx_in_parent):
    m_size(0), m_top(alloc), m_columns(alloc), m_spec_set(alloc), m_ref_count(0)
{
    // Load from allocated memory
    m_top.UpdateRef(top_ref);
    m_top.SetParent(parent, ndx_in_parent);
    assert(m_top.Size() == 2);

    const size_t schema_ref  = m_top.GetAsRef(0);
    const size_t columns_ref = m_top.GetAsRef(1);

    Create(schema_ref, columns_ref, &m_top, 1);
    m_spec_set.SetParent(&m_top, 0);
}

// Create attached sub-table from ref and schema_ref
Table::Table(SubtableTag, Allocator& alloc, size_t schema_ref, size_t columns_ref,
             Parent* parent, size_t ndx_in_parent):
    m_size(0), m_top(alloc), m_columns(alloc), m_spec_set(alloc), m_ref_count(0)
{
    Create(schema_ref, columns_ref, parent, ndx_in_parent);
}

void Table::Create(size_t ref_specSet, size_t columns_ref,
                   ArrayParent *parent, size_t ndx_in_parent)
{
    m_spec_set.UpdateRef(ref_specSet);

    // A table instatiated with a zero-ref is just an empty table
    // but it will have to create itself on first modification
    if (columns_ref != 0) {
        m_columns.UpdateRef(columns_ref);
        CacheColumns();
    }
    m_columns.SetParent(parent, ndx_in_parent);
}

void Table::CreateColumns()
{
    assert(!m_columns.IsValid() || m_columns.IsEmpty()); // only on initial creation

    // Instantiate first if we have an empty table (from zero-ref)
    if (!m_columns.IsValid()) {
        m_columns.SetType(COLUMN_HASREFS);
    }
	
    size_t subtable_count = 0;
    ColumnType attr = COLUMN_ATTR_NONE;
    Allocator& alloc = m_columns.GetAllocator();
    const size_t count = m_spec_set.GetTypeAttrCount();

    // Add the newly defined columns
    for (size_t i = 0; i < count; ++i) {
        const ColumnType type = m_spec_set.GetTypeAttr(i);
        const size_t ref_pos =  m_columns.Size();
        ColumnBase* newColumn = NULL;

        switch (type) {
            case COLUMN_TYPE_INT:
            case COLUMN_TYPE_BOOL:
            case COLUMN_TYPE_DATE:
                newColumn = new Column(COLUMN_NORMAL, alloc);
                m_columns.Add(((Column*)newColumn)->GetRef());
                ((Column*)newColumn)->SetParent(&m_columns, ref_pos);
                break;
            case COLUMN_TYPE_STRING:
                newColumn = new AdaptiveStringColumn(alloc);
                m_columns.Add(((AdaptiveStringColumn*)newColumn)->GetRef());
                ((Column*)newColumn)->SetParent(&m_columns, ref_pos);
                break;
            case COLUMN_TYPE_BINARY:
                newColumn = new ColumnBinary(alloc);
                m_columns.Add(((ColumnBinary*)newColumn)->GetRef());
                ((ColumnBinary*)newColumn)->SetParent(&m_columns, ref_pos);
                break;
            case COLUMN_TYPE_TABLE:
            {
                const size_t subspec_ref = m_spec_set.GetSubSpecRef(subtable_count);
                newColumn = new ColumnTable(subspec_ref, NULL, 0, alloc, this);
                m_columns.Add(((ColumnTable*)newColumn)->GetRef());
                ((ColumnTable*)newColumn)->SetParent(&m_columns, ref_pos);
                ++subtable_count;
            }
                break;
            case COLUMN_TYPE_MIXED:
                newColumn = new ColumnMixed(alloc, this);
                m_columns.Add(((ColumnMixed*)newColumn)->GetRef());
                ((ColumnMixed*)newColumn)->SetParent(&m_columns, ref_pos);
                break;

            // Attributes
            case COLUMN_ATTR_INDEXED:
            case COLUMN_ATTR_UNIQUE:
                attr = type;
                break;

            default:
                assert(false);
        }
		
        // Atributes on columns may define that they come with an index
        if (attr != COLUMN_ATTR_NONE) {
            assert(false); //TODO: 
            //const index_ref = newColumn->CreateIndex(attr);
            //m_columns.Add(index_ref); 

            attr = COLUMN_ATTR_NONE;
        }

        // Cache Columns
        m_cols.Add((intptr_t)newColumn);
    }
}

Spec& Table::GetSpec()
{
    assert(m_top.IsValid()); // you can only change specs on top-level tablesu
    return m_spec_set;
}

const Spec& Table::GetSpec() const
{
    return m_spec_set;
}


void Table::InstantiateBeforeChange()
{
    // Empty (zero-ref'ed) tables need to be instantiated before first modification
    if (!m_columns.IsValid()) CreateColumns();
}

void Table::CacheColumns()
{
    assert(m_cols.IsEmpty()); // only done on creation

    Allocator& alloc = m_columns.GetAllocator();
    ColumnType attr = COLUMN_ATTR_NONE;
    size_t size = (size_t)-1;
    size_t column_ndx = 0;
    const size_t count = m_spec_set.GetTypeAttrCount();
    size_t subtable_count = 0;

    // Cache columns
    for (size_t i = 0; i < count; ++i) {
        const ColumnType type = m_spec_set.GetTypeAttr(i);
        const size_t ref = m_columns.GetAsRef(column_ndx);

        ColumnBase* newColumn = NULL;
        size_t colsize = (size_t)-1;
        switch (type) {
            case COLUMN_TYPE_INT:
            case COLUMN_TYPE_BOOL:
            case COLUMN_TYPE_DATE:
                newColumn = new Column(ref, &m_columns, column_ndx, alloc);
                colsize = ((Column*)newColumn)->Size();
                break;
            case COLUMN_TYPE_STRING:
                newColumn = new AdaptiveStringColumn(ref, &m_columns, column_ndx, alloc);
                colsize = ((AdaptiveStringColumn*)newColumn)->Size();
                break;
            case COLUMN_TYPE_BINARY:
                newColumn = new ColumnBinary(ref, &m_columns, column_ndx, alloc);
                colsize = ((ColumnBinary*)newColumn)->Size();
                break;
            case COLUMN_TYPE_STRING_ENUM:
            {
                const size_t ref_values = m_columns.GetAsRef(column_ndx+1);
                newColumn = new ColumnStringEnum(ref, ref_values, &m_columns, column_ndx, alloc);
                colsize = ((ColumnStringEnum*)newColumn)->Size();
                ++column_ndx; // advance one extra pos to account for keys/values pair
                break;
            }
            case COLUMN_TYPE_TABLE:
            {
                const size_t ref_specSet = m_spec_set.GetSubSpecRef(subtable_count);

                newColumn = new ColumnTable(ref, ref_specSet, &m_columns, column_ndx, alloc, this);
                colsize = ((ColumnTable*)newColumn)->Size();
                ++subtable_count;
                break;
            }
            case COLUMN_TYPE_MIXED:
                newColumn = new ColumnMixed(ref, &m_columns, column_ndx, alloc, this);
                colsize = ((ColumnMixed*)newColumn)->Size();
                break;
				
            // Attributes
            case COLUMN_ATTR_INDEXED:
            case COLUMN_ATTR_UNIQUE:
                attr = type;
                break;

            default:
                assert(false);
        }

        m_cols.Add((intptr_t)newColumn);
		
        // Atributes on columns may define that they come with an index
        if (attr != COLUMN_ATTR_NONE) {
            const size_t index_ref = m_columns.GetAsRef(column_ndx+1);
            newColumn->SetIndexRef(index_ref);

            ++column_ndx; // advance one extra pos to account for index
            attr = COLUMN_ATTR_NONE;
        }

        // Set table size
        // (and verify that all column are same length)
        if (size == (size_t)-1) size = colsize;
        else assert(size == colsize);

        ++column_ndx;
    }

    if (size != (size_t)-1) m_size = size;
}

void Table::ClearCachedColumns()
{
    assert(m_cols.IsValid());

    const size_t count = m_cols.Size();
    for (size_t i = 0; i < count; ++i) {
        const ColumnType type = GetRealColumnType(i);
        if (type == COLUMN_TYPE_STRING_ENUM) {
            ColumnStringEnum* const column = (ColumnStringEnum* const)m_cols.Get(i);
            delete(column);
        }
        else {
            ColumnBase* const column = (ColumnBase* const)m_cols.Get(i);
            delete(column);
        }
    }
    m_cols.Destroy();
}

Table::~Table()
{
    // Delete cached columns
    ClearCachedColumns();

    if (m_top.IsValid()) {
        // 'm_top' has no parent if, and only if this is a free
        // standing instance of TopLevelTable. In that case it is the
        // responsibility of this destructor to deallocate all the
        // memory chunks that make up the entire hierarchy of
        // arrays. Otherwise we must notify our parent.
        if (ArrayParent *parent = m_top.GetParent()) {
            assert(m_ref_count == 0 || m_ref_count == 1);
            assert(dynamic_cast<Parent *>(parent));
            static_cast<Parent *>(parent)->child_destroyed(m_top.GetParentNdx());
            return;
        }

        assert(m_ref_count == 1);
        m_top.Destroy();
        return;
    }

    // 'm_columns' has no parent if, and only if this is a free
    // standing instance of Table. In that case it is the
    // responsibility of this destructor to deallocate all the memory
    // chunks that make up the entire hierarchy of arrays. Otherwise
    // we must notify our parent.
    if (ArrayParent *parent = m_columns.GetParent()) {
        assert(m_ref_count == 0 || m_ref_count == 1);
        assert(dynamic_cast<Parent *>(parent));
        static_cast<Parent *>(parent)->child_destroyed(m_columns.GetParentNdx());
        return;
    }

    assert(m_ref_count == 1);
    m_spec_set.Destroy();
    m_columns.Destroy();
}

size_t Table::GetColumnCount() const
{
    return m_spec_set.GetColumnCount();
}

const char* Table::GetColumnName(size_t ndx) const
{
    assert(ndx < GetColumnCount());
    return m_spec_set.GetColumnName(ndx);
}

size_t Table::GetColumnIndex(const char* name) const
{
    return m_spec_set.GetColumnIndex(name);
}

ColumnType Table::GetRealColumnType(size_t ndx) const
{
    assert(ndx < GetColumnCount());
    return m_spec_set.GetRealColumnType(ndx);
}

ColumnType Table::GetColumnType(size_t ndx) const
{
    assert(ndx < GetColumnCount());

    // Hides internal types like COLUM_STRING_ENUM
    return m_spec_set.GetColumnType(ndx);
}

size_t Table::GetColumnRefPos(size_t column_ndx) const
{
    size_t pos = 0;
    size_t current_column = 0;
    const size_t count = m_spec_set.GetTypeAttrCount();

    for (size_t i = 0; i < count; ++i) {
        if (current_column == column_ndx) return pos;

        const ColumnType type = (ColumnType)m_spec_set.GetTypeAttr(i);
        if (type >= COLUMN_ATTR_INDEXED) continue; // ignore attributes
        if (type < COLUMN_TYPE_STRING_ENUM) ++pos;
        else pos += 2;

        ++current_column;
    }

    assert(false);
    return (size_t)-1;
}

size_t Table::register_column(ColumnType type, const char* name)
{
    const size_t column_ndx = m_cols.Size();

    ColumnBase* newColumn = NULL;
    Allocator& alloc = m_columns.GetAllocator();

    switch (type) {
    case COLUMN_TYPE_INT:
    case COLUMN_TYPE_BOOL:
    case COLUMN_TYPE_DATE:
        newColumn = new Column(COLUMN_NORMAL, alloc);
        m_columns.Add(((Column*)newColumn)->GetRef());
        ((Column*)newColumn)->SetParent(&m_columns, m_columns.Size()-1);
        break;
    case COLUMN_TYPE_STRING:
        newColumn = new AdaptiveStringColumn(alloc);
        m_columns.Add(((AdaptiveStringColumn*)newColumn)->GetRef());
        ((Column*)newColumn)->SetParent(&m_columns, m_columns.Size()-1);
        break;
    case COLUMN_TYPE_BINARY:
        newColumn = new ColumnBinary(alloc);
        m_columns.Add(((ColumnBinary*)newColumn)->GetRef());
        ((ColumnBinary*)newColumn)->SetParent(&m_columns, m_columns.Size()-1);
        break;
    case COLUMN_TYPE_MIXED:
        newColumn = new ColumnMixed(alloc, this);
        m_columns.Add(((ColumnMixed*)newColumn)->GetRef());
        ((ColumnMixed*)newColumn)->SetParent(&m_columns, m_columns.Size()-1);
        break;
    default:
        assert(false);
    }

    m_spec_set.AddColumn(type, name);
    m_cols.Add((intptr_t)newColumn);

    return column_ndx;
}

bool Table::HasIndex(size_t column_id) const
{
    assert(column_id < GetColumnCount());
    const ColumnBase& col = GetColumnBase(column_id);
    return col.HasIndex();
}

void Table::SetIndex(size_t column_id)
{
    assert(column_id < GetColumnCount());
    if (HasIndex(column_id)) return;

    ColumnBase& col = GetColumnBase(column_id);

    if (col.IsIntColumn()) {
        Column& c = static_cast<Column&>(col);
        Index* index = new Index();
        c.BuildIndex(*index);
        m_columns.Add((intptr_t)index->GetRef());
    }
    else {
        assert(false);
    }
}

ColumnBase& Table::GetColumnBase(size_t ndx)
{
    assert(ndx < GetColumnCount());
    InstantiateBeforeChange();
    return *(ColumnBase* const)m_cols.Get(ndx);
}

const ColumnBase& Table::GetColumnBase(size_t ndx) const
{
    assert(ndx < GetColumnCount());
    return *(const ColumnBase* const)m_cols.Get(ndx);
}

Column& Table::GetColumn(size_t ndx)
{
    ColumnBase& column = GetColumnBase(ndx);
    assert(column.IsIntColumn());
    return static_cast<Column&>(column);
}

const Column& Table::GetColumn(size_t ndx) const
{
    const ColumnBase& column = GetColumnBase(ndx);
    assert(column.IsIntColumn());
    return static_cast<const Column&>(column);
}

AdaptiveStringColumn& Table::GetColumnString(size_t ndx)
{
    ColumnBase& column = GetColumnBase(ndx);
    assert(column.IsStringColumn());
    return static_cast<AdaptiveStringColumn&>(column);
}

const AdaptiveStringColumn& Table::GetColumnString(size_t ndx) const
{
    const ColumnBase& column = GetColumnBase(ndx);
    assert(column.IsStringColumn());
    return static_cast<const AdaptiveStringColumn&>(column);
}


ColumnStringEnum& Table::GetColumnStringEnum(size_t ndx)
{
    assert(ndx < GetColumnCount());
    InstantiateBeforeChange();
    return *(ColumnStringEnum* const)m_cols.Get(ndx);
}

const ColumnStringEnum& Table::GetColumnStringEnum(size_t ndx) const
{
    assert(ndx < GetColumnCount());
    return *(const ColumnStringEnum* const)m_cols.Get(ndx);
}

ColumnBinary& Table::GetColumnBinary(size_t ndx)
{
    ColumnBase& column = GetColumnBase(ndx);
    assert(column.IsBinaryColumn());
    return static_cast<ColumnBinary&>(column);
}

const ColumnBinary& Table::GetColumnBinary(size_t ndx) const
{
    const ColumnBase& column = GetColumnBase(ndx);
    assert(column.IsBinaryColumn());
    return static_cast<const ColumnBinary&>(column);
}

ColumnTable &Table::GetColumnTable(size_t ndx)
{
    assert(ndx < GetColumnCount());
    InstantiateBeforeChange();
    return *reinterpret_cast<ColumnTable *>(m_cols.Get(ndx));
}

ColumnTable const &Table::GetColumnTable(size_t ndx) const
{
    assert(ndx < GetColumnCount());
    return *reinterpret_cast<ColumnTable *>(m_cols.Get(ndx));
}

ColumnMixed& Table::GetColumnMixed(size_t ndx)
{
    assert(ndx < GetColumnCount());
    InstantiateBeforeChange();
    return *(ColumnMixed* const)m_cols.Get(ndx);
}

const ColumnMixed& Table::GetColumnMixed(size_t ndx) const
{
    assert(ndx < GetColumnCount());
    return *(const ColumnMixed* const)m_cols.Get(ndx);
}

size_t Table::AddRow()
{
    const size_t count = GetColumnCount();
    for (size_t i = 0; i < count; ++i) {
        ColumnBase& column = GetColumnBase(i);
        column.Add();
    }

    return m_size++;
}

void Table::Clear()
{
    const size_t count = GetColumnCount();
    for (size_t i = 0; i < count; ++i) {
        ColumnBase& column = GetColumnBase(i);
        column.Clear();
    }
    m_size = 0;
}

void Table::DeleteRow(size_t ndx)
{
    assert(ndx < m_size);

    const size_t count = GetColumnCount();
    for (size_t i = 0; i < count; ++i) {
        ColumnBase& column = GetColumnBase(i);
        column.Delete(ndx);
    }
    --m_size;
}

void Table::InsertTable(size_t column_id, size_t ndx)
{
    assert(column_id < GetColumnCount());
    assert(GetRealColumnType(column_id) == COLUMN_TYPE_TABLE);
    assert(ndx <= m_size);

    ColumnTable& subtables = GetColumnTable(column_id);
    subtables.Insert(ndx);
}

void Table::ClearTable(size_t column_id, size_t ndx)
{
    assert(column_id < GetColumnCount());
    assert(GetRealColumnType(column_id) == COLUMN_TYPE_TABLE);
    assert(ndx <= m_size);

    ColumnTable& subtables = GetColumnTable(column_id);
    subtables.Clear(ndx);
}

Table* Table::get_subtable_ptr(size_t col_idx, size_t row_idx)
{
    assert(col_idx < GetColumnCount());
    assert(row_idx < m_size);

    const ColumnType type = GetRealColumnType(col_idx);
    if (type == COLUMN_TYPE_TABLE) {
        ColumnTable& subtables = GetColumnTable(col_idx);
        return subtables.get_subtable_ptr(row_idx);
    }
    else if (type == COLUMN_TYPE_MIXED) {
        ColumnMixed& subtables = GetColumnMixed(col_idx);
        return subtables.get_subtable_ptr(row_idx);
    }
    else {
        assert(false);
        return 0;
    }
}

const Table* Table::get_subtable_ptr(size_t col_idx, size_t row_idx) const
{
    assert(col_idx < GetColumnCount());
    assert(row_idx < m_size);

    const ColumnType type = GetRealColumnType(col_idx);
    if (type == COLUMN_TYPE_TABLE) {
        const ColumnTable& subtables = GetColumnTable(col_idx);
        return subtables.get_subtable_ptr(row_idx);
    }
    else if (type == COLUMN_TYPE_MIXED) {
        const ColumnMixed& subtables = GetColumnMixed(col_idx);
        return subtables.get_subtable_ptr(row_idx);
    }
    else {
        assert(false);
        return 0;
    }
}

size_t Table::GetTableSize(size_t column_id, size_t ndx) const
{
    assert(column_id < GetColumnCount());
    assert(GetRealColumnType(column_id) == COLUMN_TYPE_TABLE);
    assert(ndx < m_size);

    // FIXME: Should also be made to work for ColumnMixed
    ColumnTable const &subtables = GetColumnTable(column_id);
    return subtables.GetTableSize(ndx);
}

int64_t Table::Get(size_t column_id, size_t ndx) const
{
    assert(column_id < GetColumnCount());
    assert(ndx < m_size);

    const Column& column = GetColumn(column_id);
    return column.Get(ndx);
}

void Table::Set(size_t column_id, size_t ndx, int64_t value)
{
    assert(column_id < GetColumnCount());
    assert(ndx < m_size);

    Column& column = GetColumn(column_id);
    column.Set(ndx, value);
}

bool Table::GetBool(size_t column_id, size_t ndx) const
{
    assert(column_id < GetColumnCount());
    assert(GetRealColumnType(column_id) == COLUMN_TYPE_BOOL);
    assert(ndx < m_size);

    const Column& column = GetColumn(column_id);
    return column.Get(ndx) != 0;
}

void Table::SetBool(size_t column_id, size_t ndx, bool value)
{
    assert(column_id < GetColumnCount());
    assert(GetRealColumnType(column_id) == COLUMN_TYPE_BOOL);
    assert(ndx < m_size);

    Column& column = GetColumn(column_id);
    column.Set(ndx, value ? 1 : 0);
}

time_t Table::GetDate(size_t column_id, size_t ndx) const
{
    assert(column_id < GetColumnCount());
    assert(GetRealColumnType(column_id) == COLUMN_TYPE_DATE);
    assert(ndx < m_size);

    const Column& column = GetColumn(column_id);
    return (time_t)column.Get(ndx);
}

void Table::SetDate(size_t column_id, size_t ndx, time_t value)
{
    assert(column_id < GetColumnCount());
    assert(GetRealColumnType(column_id) == COLUMN_TYPE_DATE);
    assert(ndx < m_size);

    Column& column = GetColumn(column_id);
    column.Set(ndx, (int64_t)value);
}

void Table::InsertInt(size_t column_id, size_t ndx, int64_t value)
{
    assert(column_id < GetColumnCount());
    assert(ndx <= m_size);

    Column& column = GetColumn(column_id);
    column.Insert(ndx, value);
}

const char* Table::GetString(size_t column_id, size_t ndx) const
{
    assert(column_id < m_columns.Size());
    assert(ndx < m_size);

    const ColumnType type = GetRealColumnType(column_id);

    if (type == COLUMN_TYPE_STRING) {
        const AdaptiveStringColumn& column = GetColumnString(column_id);
        return column.Get(ndx);
    }
    else {
        assert(type == COLUMN_TYPE_STRING_ENUM);
        const ColumnStringEnum& column = GetColumnStringEnum(column_id);
        return column.Get(ndx);
    }
}

void Table::SetString(size_t column_id, size_t ndx, const char* value)
{
    assert(column_id < GetColumnCount());
    assert(ndx < m_size);

    const ColumnType type = GetRealColumnType(column_id);

    if (type == COLUMN_TYPE_STRING) {
        AdaptiveStringColumn& column = GetColumnString(column_id);
        column.Set(ndx, value);
    }
    else {
        assert(type == COLUMN_TYPE_STRING_ENUM);
        ColumnStringEnum& column = GetColumnStringEnum(column_id);
        column.Set(ndx, value);
    }
}

void Table::InsertString(size_t column_id, size_t ndx, const char* value)
{
    assert(column_id < GetColumnCount());
    assert(ndx <= m_size);

    const ColumnType type = GetRealColumnType(column_id);

    if (type == COLUMN_TYPE_STRING) {
        AdaptiveStringColumn& column = GetColumnString(column_id);
        column.Insert(ndx, value);
    }
    else {
        assert(type == COLUMN_TYPE_STRING_ENUM);
        ColumnStringEnum& column = GetColumnStringEnum(column_id);
        column.Insert(ndx, value);
    }
}

BinaryData Table::GetBinary(size_t column_id, size_t ndx) const
{
    assert(column_id < m_columns.Size());
    assert(ndx < m_size);

    const ColumnBinary& column = GetColumnBinary(column_id);
    return column.Get(ndx);
}

void Table::SetBinary(size_t column_id, size_t ndx, const char* value, size_t len)
{
    assert(column_id < GetColumnCount());
    assert(ndx < m_size);

    ColumnBinary& column = GetColumnBinary(column_id);
    column.Set(ndx, value, len);
}

void Table::InsertBinary(size_t column_id, size_t ndx, const char* value, size_t len)
{
    assert(column_id < GetColumnCount());
    assert(ndx <= m_size);

    ColumnBinary& column = GetColumnBinary(column_id);
    column.Insert(ndx, value, len);
}

Mixed Table::GetMixed(size_t column_id, size_t ndx) const
{
    assert(column_id < m_columns.Size());
    assert(ndx < m_size);

    const ColumnMixed& column = GetColumnMixed(column_id);
    const ColumnType   type   = column.GetType(ndx);

    switch (type) {
        case COLUMN_TYPE_INT:
            return Mixed(column.GetInt(ndx));
        case COLUMN_TYPE_BOOL:
            return Mixed(column.GetBool(ndx));
        case COLUMN_TYPE_DATE:
            return Mixed(Date(column.GetDate(ndx)));
        case COLUMN_TYPE_STRING:
            return Mixed(column.GetString(ndx));
        case COLUMN_TYPE_BINARY:
            return Mixed(column.GetBinary(ndx));
        case COLUMN_TYPE_TABLE:
            return Mixed(COLUMN_TYPE_TABLE);
        default:
            assert(false);
            return Mixed((int64_t)0);
    }
}

ColumnType Table::GetMixedType(size_t column_id, size_t ndx) const
{
    assert(column_id < m_columns.Size());
    assert(ndx < m_size);

    const ColumnMixed& column = GetColumnMixed(column_id);
    return column.GetType(ndx);
}

void Table::SetMixed(size_t column_id, size_t ndx, Mixed value)
{
    assert(column_id < GetColumnCount());
    assert(ndx < m_size);

    ColumnMixed& column = GetColumnMixed(column_id);
    const ColumnType type = value.GetType();

    switch (type) {
        case COLUMN_TYPE_INT:
            column.SetInt(ndx, value.GetInt());
            break;
        case COLUMN_TYPE_BOOL:
            column.SetBool(ndx, value.GetBool());
            break;
        case COLUMN_TYPE_DATE:
            column.SetDate(ndx, value.GetDate());
            break;
        case COLUMN_TYPE_STRING:
            column.SetString(ndx, value.GetString());
            break;
        case COLUMN_TYPE_BINARY:
        {
            const BinaryData b = value.GetBinary();
            column.SetBinary(ndx, (const char*)b.pointer, b.len);
            break;
        }
        case COLUMN_TYPE_TABLE:
            column.SetTable(ndx);
            break;
        default:
            assert(false);
    }
}

void Table::InsertMixed(size_t column_id, size_t ndx, Mixed value) {
    assert(column_id < GetColumnCount());
    assert(ndx <= m_size);

    ColumnMixed& column = GetColumnMixed(column_id);
    const ColumnType type = value.GetType();

    switch (type) {
        case COLUMN_TYPE_INT:
            column.InsertInt(ndx, value.GetInt());
            break;
        case COLUMN_TYPE_BOOL:
            column.InsertBool(ndx, value.GetBool());
            break;
        case COLUMN_TYPE_DATE:
            column.InsertDate(ndx, value.GetDate());
            break;
        case COLUMN_TYPE_STRING:
            column.InsertString(ndx, value.GetString());
            break;
        case COLUMN_TYPE_BINARY:
        {
            const BinaryData b = value.GetBinary();
            column.InsertBinary(ndx, (const char*)b.pointer, b.len);
            break;
        }
        case COLUMN_TYPE_TABLE:
            column.InsertTable(ndx);
            break;
        default:
            assert(false);
    }
}

void Table::InsertDone()
{
    ++m_size;

#ifdef _DEBUG
    verify();
#endif //_DEBUG
}

size_t Table::Find(size_t column_id, int64_t value) const
{
    assert(column_id < m_columns.Size());
    assert(GetRealColumnType(column_id) == COLUMN_TYPE_INT);
    const Column& column = GetColumn(column_id);

    return column.Find(value);
}

size_t Table::FindBool(size_t column_id, bool value) const
{
    assert(column_id < m_columns.Size());
    assert(GetRealColumnType(column_id) == COLUMN_TYPE_BOOL);
    const Column& column = GetColumn(column_id);

    return column.Find(value ? 1 : 0);
}

size_t Table::FindDate(size_t column_id, time_t value) const
{
    assert(column_id < m_columns.Size());
    assert(GetRealColumnType(column_id) == COLUMN_TYPE_DATE);
    const Column& column = GetColumn(column_id);

    return column.Find((int64_t)value);
}

size_t Table::FindString(size_t column_id, const char* value) const
{
    assert(column_id < m_columns.Size());

    const ColumnType type = GetRealColumnType(column_id);

    if (type == COLUMN_TYPE_STRING) {
        const AdaptiveStringColumn& column = GetColumnString(column_id);
        return column.Find(value);
    }
    else {
        assert(type == COLUMN_TYPE_STRING_ENUM);
        const ColumnStringEnum& column = GetColumnStringEnum(column_id);
        return column.Find(value);
    }
}

void Table::FindAll(TableView& tv, size_t column_id, int64_t value)
{
    assert(column_id < m_columns.Size());
    assert(&tv.GetParent() == this);

    const Column& column = GetColumn(column_id);

    column.FindAll(tv.GetRefColumn(), value);
}

void Table::FindAllBool(TableView& tv, size_t column_id, bool value)
{
    assert(column_id < m_columns.Size());
    assert(&tv.GetParent() == this);

    const Column& column = GetColumn(column_id);

    column.FindAll(tv.GetRefColumn(), value ? 1 :0);
}

void Table::FindAllString(TableView& tv, size_t column_id, const char *value)
{
    assert(column_id < m_columns.Size());
    assert(&tv.GetParent() == this);

    const ColumnType type = GetRealColumnType(column_id);

    if (type == COLUMN_TYPE_STRING) {
        const AdaptiveStringColumn& column = GetColumnString(column_id);
        column.FindAll(tv.GetRefColumn(), value);
    }
    else {
        assert(type == COLUMN_TYPE_STRING_ENUM);
        const ColumnStringEnum& column = GetColumnStringEnum(column_id);
        column.FindAll(tv.GetRefColumn(), value);
    }
}



void Table::FindAllHamming(TableView& tv, size_t column_id, uint64_t value, size_t max)
{
    assert(column_id < m_columns.Size());
    assert(&tv.GetParent() == this);

    const Column& column = GetColumn(column_id);

    column.FindAllHamming(tv.GetRefColumn(), value, max);
}

void Table::Optimize()
{
    const size_t column_count = GetColumnCount();
    Allocator& alloc = m_columns.GetAllocator();

    for (size_t i = 0; i < column_count; ++i) {
        const ColumnType type = GetRealColumnType(i);

        if (type == COLUMN_TYPE_STRING) {
            AdaptiveStringColumn& column = GetColumnString(i);

            size_t ref_keys;
            size_t ref_values;
            const bool res = column.AutoEnumerate(ref_keys, ref_values);
            if (!res) continue;

            // Add to spec and column refs
            m_spec_set.SetColumnType(i, COLUMN_TYPE_STRING_ENUM);
            const size_t column_ndx = GetColumnRefPos(i);
            m_columns.Set(column_ndx, ref_keys);
            m_columns.Insert(column_ndx+1, ref_values);

            // There are still same number of columns, but since
            // the enum type takes up two posistions in m_columns
            // we have to move refs in all following columns
            UpdateColumnRefs(column_ndx+1, 1);

            // Replace cached column
            ColumnStringEnum* e = new ColumnStringEnum(ref_keys, ref_values, &m_columns, column_ndx, alloc);
            m_cols.Set(i, (intptr_t)e);
            column.Destroy();
            delete &column;
        }
    }
}

void Table::UpdateColumnRefs(size_t column_ndx, int diff)
{
    for (size_t i = column_ndx; i < m_cols.Size(); ++i) {
        ColumnBase* const column = (ColumnBase*)m_cols.Get(i);
        column->UpdateParentNdx(diff);
    }
}
    
void Table::UpdateFromParent() {
    // There is no top for sub-tables sharing schema
    if (m_top.IsValid()) {
        if (!m_top.UpdateFromParent()) return;
    }
    
    m_spec_set.UpdateFromParent();
    if (!m_columns.UpdateFromParent()) return;
    
    // Update cached columns
    const size_t column_count = GetColumnCount();
    for (size_t i = 0; i < column_count; ++i) {
        ColumnBase* const column = (ColumnBase*)m_cols.Get(i);
        column->UpdateFromParent();
    }
}


void Table::UpdateFromSpec()
{
    assert(m_columns.IsEmpty() && m_cols.IsEmpty()); // only on initial creation

    CreateColumns();
}


size_t Table::create_table(Allocator& alloc)
{
    FakeParent fake_parent;
    Table t(alloc);
    t.m_top.SetParent(&fake_parent, 0);
    return t.m_top.GetRef();
}


void Table::to_json(std::ostream& out)
{
    // Represent table as list of objects
    out << "[";

    const size_t row_count    = GetSize();
    const size_t column_count = GetColumnCount();

    // We need a buffer for formatting dates (and binary to hex). Max
    // size is 21 bytes (incl quotes and zero byte) "YYYY-MM-DD HH:MM:SS"\0
    char buffer[30];

    for (size_t r = 0; r < row_count; ++r) {
        if (r) out << ",";
        out << "{";

        for (size_t i = 0; i < column_count; ++i) {
            if (i) out << ",";

            const char* const name = GetColumnName(i);
            out << "\"" << name << "\":";

            const ColumnType type = GetColumnType(i);
            switch (type) {
                case COLUMN_TYPE_INT:
                    out << Get(i, r);
                    break;
                case COLUMN_TYPE_BOOL:
                    out << (GetBool(i, r) ? "true" : "false");
                    break;
                case COLUMN_TYPE_STRING:
                    out << "\"" << GetString(i, r) << "\"";
                    break;
                case COLUMN_TYPE_DATE:
                {
                    const time_t rawtime = GetDate(i, r);
                    struct tm* const t = gmtime(&rawtime);
                    const size_t res = strftime(buffer, 30, "\"%Y-%m-%d %H:%M:%S\"", t);
                    if (!res) break;

                    out << buffer;
                    break;
                }
                case COLUMN_TYPE_BINARY:
                {
                    const BinaryData bin = GetBinary(i, r);
                    const char* const p = (char*)bin.pointer;

                    out << "\"";
                    for (size_t i = 0; i < bin.len; ++i) {
                        sprintf(buffer, "%02x", (unsigned int)p[i]);
                        out << buffer;
                    }
                    out << "\"";
                    break;
                }
                case COLUMN_TYPE_TABLE:
                {
                    GetTable(i, r)->to_json(out);
                    break;
                }
                case COLUMN_TYPE_MIXED:
                {
                    const ColumnType mtype = GetMixedType(i, r);
                    if (mtype == COLUMN_TYPE_TABLE) {
                        GetTable(i, r)->to_json(out);
                    }
                    else {
                        const Mixed m = GetMixed(i, r);
                        switch (mtype) {
                            case COLUMN_TYPE_INT:
                                out << m.GetInt();
                                break;
                            case COLUMN_TYPE_BOOL:
                                out << m.GetBool();
                                break;
                            case COLUMN_TYPE_STRING:
                                out << "\"" << m.GetString() << "\"";
                                break;
                            case COLUMN_TYPE_DATE:
                            {
                                const time_t rawtime = m.GetDate();
                                struct tm* const t = gmtime(&rawtime);
                                const size_t res = strftime(buffer, 30, "\"%Y-%m-%d %H:%M:%S\"", t);
                                if (!res) break;

                                out << buffer;
                                break;
                            }
                            case COLUMN_TYPE_BINARY:
                            {
                                const BinaryData bin = m.GetBinary();
                                const char* const p = (char*)bin.pointer;

                                out << "\"";
                                for (size_t i = 0; i < bin.len; ++i) {
                                    sprintf(buffer, "%02x", (unsigned int)p[i]);
                                    out << buffer;
                                }
                                out << "\"";
                                break;
                            }
                            default:
                                assert(false);
                        }

                    }
                    break;
                }

                default:
                    assert(false);
            }
        }

        out << "}";
    }

    out << "]";
}

#ifdef _DEBUG

bool Table::Compare(const Table& c) const
{
    if (!m_spec_set.Compare(c.m_spec_set)) return false;

    const size_t column_count = GetColumnCount();
    if (column_count != c.GetColumnCount()) return false;

    for (size_t i = 0; i < column_count; ++i) {
        const ColumnType type = GetRealColumnType(i);

        switch (type) {
            case COLUMN_TYPE_INT:
            case COLUMN_TYPE_BOOL:
                {
                    const Column& column1 = GetColumn(i);
                    const Column& column2 = c.GetColumn(i);
                    if (!column1.Compare(column2)) return false;
                }
                break;
            case COLUMN_TYPE_STRING:
                {
                    const AdaptiveStringColumn& column1 = GetColumnString(i);
                    const AdaptiveStringColumn& column2 = c.GetColumnString(i);
                    if (!column1.Compare(column2)) return false;
                }
                break;
            case COLUMN_TYPE_STRING_ENUM:
                {
                    const ColumnStringEnum& column1 = GetColumnStringEnum(i);
                    const ColumnStringEnum& column2 = c.GetColumnStringEnum(i);
                    if (!column1.Compare(column2)) return false;
                }
                break;

            default:
                assert(false);
        }
    }
    return true;
}

void Table::verify() const
{
    const size_t column_count = GetColumnCount();
    assert(column_count == m_cols.Size());

    for (size_t i = 0; i < column_count; ++i) {
        const ColumnType type = GetRealColumnType(i);
        switch (type) {
        case COLUMN_TYPE_INT:
        case COLUMN_TYPE_BOOL:
        case COLUMN_TYPE_DATE:
            {
                const Column& column = GetColumn(i);
                assert(column.Size() == m_size);
                column.verify();
            }
            break;
        case COLUMN_TYPE_STRING:
            {
                const AdaptiveStringColumn& column = GetColumnString(i);
                assert(column.Size() == m_size);
                column.verify();
            }
            break;
        case COLUMN_TYPE_STRING_ENUM:
            {
                const ColumnStringEnum& column = GetColumnStringEnum(i);
                assert(column.Size() == m_size);
                column.verify();
            }
            break;
        case COLUMN_TYPE_BINARY:
            {
                const ColumnBinary& column = GetColumnBinary(i);
                assert(column.Size() == m_size);
                column.verify();
            }
            break;
        case COLUMN_TYPE_TABLE:
            {
                const ColumnTable& column = GetColumnTable(i);
                assert(column.Size() == m_size);
                column.verify();
            }
            break;
        case COLUMN_TYPE_MIXED:
            {
                const ColumnMixed& column = GetColumnMixed(i);
                assert(column.Size() == m_size);
                column.verify();
            }
            break;
        default:
            assert(false);
        }
    }
    
    m_spec_set.Verify();

    Allocator& alloc = m_columns.GetAllocator();
    alloc.Verify();
}

void Table::ToDot(std::ostream& out, const char* title) const
{
    if (m_top.IsValid()) {
        out << "subgraph cluster_topleveltable" << m_top.GetRef() << " {" << endl;
        out << " label = \"TopLevelTable";
        if (title) out << "\\n'" << title << "'";
        out << "\";" << endl;
        m_top.ToDot(out, "table_top");
        const Spec& specset = GetSpec();
        specset.ToDot(out);
    }
    else {
        out << "subgraph cluster_table_"  << m_columns.GetRef() <<  " {" << endl;
        out << " label = \"Table";
        if (title) out << " " << title;
        out << "\";" << endl;
    }

    ToDotInternal(out);

    out << "}" << endl;
}

void Table::ToDotInternal(std::ostream& out) const
{
    m_columns.ToDot(out, "columns");

    // Columns
    const size_t column_count = GetColumnCount();
    for (size_t i = 0; i < column_count; ++i) {
        const ColumnBase& column = GetColumnBase(i);
        const char* const name = GetColumnName(i);
        column.ToDot(out, name);
    }
}

void Table::Print() const
{
    // Table header
    cout << "Table: len(" << m_size << ")\n    ";
    const size_t column_count = GetColumnCount();
    for (size_t i = 0; i < column_count; ++i) {
        const char* name = m_spec_set.GetColumnName(i);
        cout << left << setw(10) << name << right << " ";
    }

    // Types
    cout << "\n    ";
    for (size_t i = 0; i < column_count; ++i) {
        const ColumnType type = GetRealColumnType(i);
        switch (type) {
        case COLUMN_TYPE_INT:
            cout << "Int        "; break;
        case COLUMN_TYPE_BOOL:
            cout << "Bool       "; break;
        case COLUMN_TYPE_STRING:
            cout << "String     "; break;
        default:
            assert(false);
        }
    }
    cout << "\n";

    // Columns
    for (size_t i = 0; i < m_size; ++i) {
        cout << setw(3) << i;
        for (size_t n = 0; n < column_count; ++n) {
            const ColumnType type = GetRealColumnType(n);
            switch (type) {
            case COLUMN_TYPE_INT:
                {
                    const Column& column = GetColumn(n);
                    cout << setw(10) << column.Get(i) << " ";
                }
                break;
            case COLUMN_TYPE_BOOL:
                {
                    const Column& column = GetColumn(n);
                    cout << (column.Get(i) == 0 ? "     false " : "      true ");
                }
                break;
            case COLUMN_TYPE_STRING:
                {
                    const AdaptiveStringColumn& column = GetColumnString(n);
                    cout << setw(10) << column.Get(i) << " ";
                }
                break;
            default:
                assert(false);
            }
        }
        cout << "\n";
    }
    cout << "\n";
}

MemStats Table::Stats() const
{
    MemStats stats;
    m_top.Stats(stats);

    return stats;
}

#endif //_DEBUG

}
