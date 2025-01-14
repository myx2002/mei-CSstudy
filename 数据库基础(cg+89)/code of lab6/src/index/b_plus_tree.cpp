#include "index/b_plus_tree.h"

#include <string>

#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

/**
 * TODO: Student Implement
 */
BPlusTree::BPlusTree(index_id_t index_id, BufferPoolManager *buffer_pool_manager, const KeyManager &KM,
                     int leaf_max_size, int internal_max_size)
    : index_id_(index_id),
      buffer_pool_manager_(buffer_pool_manager),
      processor_(KM),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
  page_id_t page_id;
  IndexRootsPage *indexRootsPage = reinterpret_cast<IndexRootsPage *>(
      buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID));
  if(indexRootsPage->GetRootId(index_id,&page_id)){
   root_page_id_=page_id;
  }
  else{
  root_page_id_ = INVALID_PAGE_ID;

  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID,false);
}

void BPlusTree::Destroy(page_id_t current_page_id) {
  Page *page = buffer_pool_manager_->FetchPage(current_page_id);
  if (page == nullptr) {
    return;
  }
  BPlusTreePage *node = reinterpret_cast<BPlusTreePage *>(page);
  if (node->IsLeafPage()) {
    LeafPage *leaf = reinterpret_cast<LeafPage *>(page);
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    buffer_pool_manager_->DeletePage(leaf->GetPageId());
  } else {
    InternalPage *internal = reinterpret_cast<InternalPage *>(page);
    for (int i = 0; i < internal->GetSize(); i++) {
      Destroy(internal->ValueAt(i));
    }
    buffer_pool_manager_->UnpinPage(internal->GetPageId(), false);
    buffer_pool_manager_->DeletePage(internal->GetPageId());
  }
}

/*
 * Helper function to decide whether current b+tree is empty
 */
bool BPlusTree::IsEmpty() const { return root_page_id_ == INVALID_PAGE_ID; }

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
bool BPlusTree::GetValue(const GenericKey *key, std::vector<RowId> &result, Transaction *transaction) {
  if (IsEmpty()) {
    return false;
  }
  Page *page = FindLeafPage(key, root_page_id_, false);
  LeafPage *leaf = reinterpret_cast<LeafPage *>(page);
  RowId id = INVALID_ROWID;
  bool res = leaf->Lookup(key, id, processor_);
  if (res) {
    result.push_back(id);
  } else {
    int ttt = 1;
  }
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
  return res;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::Insert(GenericKey *key, const RowId &value, Transaction *transaction) {
  if (IsEmpty()) {
    StartNewTree(key, value);
    return true;
  }
  return InsertIntoLeaf(key, value, transaction);
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
void BPlusTree::StartNewTree(GenericKey *key, const RowId &value) {
  Page *page = buffer_pool_manager_->NewPage(root_page_id_);
  if (page == nullptr) {
    ASSERT(false, "all page are pinned while StartNewTree");
  }
  LeafPage *leaf = reinterpret_cast<LeafPage *>(page);
  leaf->Init(root_page_id_, INVALID_PAGE_ID, processor_.GetKeySize(), leaf_max_size_);
  leaf->Insert(key, value, processor_);
  UpdateRootPageId(1);
  buffer_pool_manager_->UnpinPage(root_page_id_, true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::InsertIntoLeaf(GenericKey *key, const RowId &value, Transaction *transaction) {
  Page *page = FindLeafPage(key, root_page_id_, false);
  LeafPage *leaf = reinterpret_cast<LeafPage *>(page);
  RowId lookup_res = INVALID_ROWID;
  if (leaf->Lookup(key, lookup_res, processor_)) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return false;
  }
  if (leaf->Insert(key, value, processor_)) {
    if (leaf->GetSize() > leaf->GetMaxSize()) {
      LeafPage *new_leaf = Split(leaf, transaction);
      InsertIntoParent(leaf, new_leaf->KeyAt(0), new_leaf, transaction);
    }
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
    return true;
  }
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
  return false;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
BPlusTreeInternalPage *BPlusTree::Split(InternalPage *node, Transaction *transaction) {
  page_id_t new_page_id;
  Page *page = buffer_pool_manager_->NewPage(new_page_id);
  if (page == nullptr) {
    ASSERT(false, "all page are pinned while Split");
  }
  InternalPage *new_page = reinterpret_cast<InternalPage *>(page);
  new_page->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), internal_max_size_);
  node->MoveHalfTo(new_page, buffer_pool_manager_);
  buffer_pool_manager_->UnpinPage(new_page_id, true);
  return new_page;
}

BPlusTreeLeafPage *BPlusTree::Split(LeafPage *node, Transaction *transaction) {
  page_id_t new_page_id;
  Page *page = buffer_pool_manager_->NewPage(new_page_id);
  if (page == nullptr) {
    ASSERT(false, "all page are pinned while Split");
  }
  LeafPage *new_page = reinterpret_cast<LeafPage *>(page);
  new_page->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), leaf_max_size_);
  node->MoveHalfTo(new_page);
  buffer_pool_manager_->UnpinPage(new_page_id, true);
  return new_page;
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key, BPlusTreePage *new_node,
                                 Transaction *transaction) {
  if (old_node->IsRootPage()) {
    page_id_t new_page_id;
    Page *page = buffer_pool_manager_->NewPage(new_page_id);
    if (page == nullptr) {
      ASSERT(false, "all page are pinned while InsertIntoParent");
    }
    InternalPage *new_page = reinterpret_cast<InternalPage *>(page);
    new_page->Init(new_page_id, INVALID_PAGE_ID, processor_.GetKeySize(), internal_max_size_);
    new_page->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
    old_node->SetParentPageId(new_page_id);
    new_node->SetParentPageId(new_page_id);
    root_page_id_ = new_page_id;
    UpdateRootPageId(0);
    buffer_pool_manager_->UnpinPage(new_page_id, true);
    return;
  }
  page_id_t parent_page_id = old_node->GetParentPageId();
  Page *page = buffer_pool_manager_->FetchPage(parent_page_id);
  if (page == nullptr) {
    ASSERT(false, "all page are pinned while InsertIntoParent");
  }
  InternalPage *parent_page = reinterpret_cast<InternalPage *>(page);
  if (parent_page->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId())) {
    if (parent_page->GetSize() > parent_page->GetMaxSize()) {
      InternalPage *new_parent_page = Split(parent_page, transaction);
      InsertIntoParent(parent_page, new_parent_page->KeyAt(0), new_parent_page, transaction);
    }
    buffer_pool_manager_->UnpinPage(parent_page_id, true);
    return;
  }
  buffer_pool_manager_->UnpinPage(parent_page_id, false);
  return;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
void BPlusTree::Remove(const GenericKey *key, Transaction *transaction) {
  if (IsEmpty()) {
    return;
  }
  Page *page = FindLeafPage(key, root_page_id_, false);
  LeafPage *leaf = reinterpret_cast<LeafPage *>(page);
  if (leaf->RemoveAndDeleteRecord(key, processor_)) {
    page_id_t now = leaf->GetPageId();
    GenericKey *new_key = leaf->KeyAt(0);
    if (!leaf->IsRootPage()) {
      int i = 0;
      InternalPage *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(leaf->GetParentPageId()));
      while (1) {
        i = parent->ValueIndex(now);
        if (i > 0) {
          parent->SetKeyAt(i, new_key);
          break;
        }
        if (parent->IsRootPage()) break;
        now = parent->GetPageId();
        parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(parent->GetParentPageId()));
      }
    }

    if (leaf->GetSize() < leaf->GetMinSize()) {
      bool t = CoalesceOrRedistribute(leaf, transaction);
      t||buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
      return;
    }
  }
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
  return;
}

/* todo
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node, Transaction *transaction) {
  if (node->IsRootPage()) {
    return AdjustRoot(node);
  }
  InternalPage *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(node->GetParentPageId()));
  int index = parent->ValueIndex(node->GetPageId());
  int sibling_index = (index == 0) ? 1 : index - 1;
  N *sibling = reinterpret_cast<N *>(buffer_pool_manager_->FetchPage(parent->ValueAt(sibling_index)));
  if(node->GetSize() >= node->GetMinSize()){
    return false;
  }
  if (sibling->GetSize() + node->GetSize() > node->GetMaxSize()) {
    Redistribute(sibling, node, parent, index);
    buffer_pool_manager_->UnpinPage(sibling->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
    return false;
  } else {
    Coalesce(sibling, node, parent, index, transaction);
    buffer_pool_manager_->UnpinPage(sibling->GetPageId(), false);
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
    return true;
  }
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion happened
 */
bool BPlusTree::Coalesce(LeafPage *&neighbor_node, LeafPage *&node, InternalPage *&parent, int index,
                         Transaction *transaction) {
  if (index == 0) {
    neighbor_node->MoveAllTo(node);
    parent->Remove(1);
    buffer_pool_manager_->DeletePage(neighbor_node->GetPageId());
  } else {
    node->MoveAllTo(neighbor_node);
    parent->Remove(index);
    buffer_pool_manager_->DeletePage(node->GetPageId());
  }
  bool t = CoalesceOrRedistribute(parent, transaction);
  t||buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  return 1;
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node, InternalPage *&node, InternalPage *&parent, int index,
                         Transaction *transaction) {
  if(index == 0) {
    neighbor_node->MoveAllTo(node, parent->KeyAt(1), buffer_pool_manager_);
    parent->Remove(1);
    buffer_pool_manager_->DeletePage(neighbor_node->GetPageId());
  } else {
    node->MoveAllTo(neighbor_node, parent->KeyAt(index), buffer_pool_manager_);
    parent->Remove(index);
    buffer_pool_manager_->DeletePage(node->GetPageId());
  }
  bool t = CoalesceOrRedistribute(parent, transaction);
  if (t) {
    buffer_pool_manager_->DeletePage(parent->GetPageId());
  } else {
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  }
  return 1;
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
void BPlusTree::Redistribute(LeafPage *neighbor_node, LeafPage *node, InternalPage *&parent, int index) {
  if (index == 0) {
    neighbor_node->MoveFirstToEndOf(node);
    parent->SetKeyAt(1, neighbor_node->KeyAt(0));
  } else {
    neighbor_node->MoveLastToFrontOf(node);
    parent->SetKeyAt(index, node->KeyAt(0));
  }
}
void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, InternalPage *&parent, int index) {
  if (index == 0) {
    neighbor_node->MoveFirstToEndOf(node, parent->KeyAt(1), buffer_pool_manager_);
    parent->SetKeyAt(1, neighbor_node->KeyAt(0)); 
  } else {
    neighbor_node->MoveLastToFrontOf(node, parent->KeyAt(index), buffer_pool_manager_);
    parent->SetKeyAt(index, neighbor_node->KeyAt(neighbor_node->GetSize()));
  }
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happened
 */
bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node) {
  if (old_root_node->IsLeafPage()) {
    LeafPage *root_node = reinterpret_cast<LeafPage *>(old_root_node);
    if (root_node->GetSize() == 0) {
      root_page_id_ = INVALID_PAGE_ID;
      UpdateRootPageId(0);
      return true;
    }
    return false;
  }
  InternalPage *root_node = reinterpret_cast<InternalPage *>(old_root_node);
  if (root_node->GetSize() == 1) {
    page_id_t new_root_id = root_node->RemoveAndReturnOnlyChild();
    // root_node->SetPageId(INVALID_PAGE_ID);
    // root_node->SetParentPageId(INVALID_PAGE_ID);
    root_page_id_ = new_root_id;
    BPlusTreePage *page = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(new_root_id));
    page->SetParentPageId(INVALID_PAGE_ID);
    buffer_pool_manager_->UnpinPage(new_root_id, true);
    UpdateRootPageId(0);
    return true;
  }
  return false;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the left most leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin() {
  page_id_t page_id = root_page_id_;
  if (page_id == INVALID_PAGE_ID) {
    return IndexIterator();
  }
  Page *page = FindLeafPage(nullptr, page_id, true);
  LeafPage *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  page_id_t leaf_id = leaf->GetPageId();
  IndexIterator iterator(leaf_id, buffer_pool_manager_, 0);
  return iterator;
}

/*
 * Input parameter is low-key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(const GenericKey *key) {
  page_id_t page_id = root_page_id_;
  if (page_id == INVALID_PAGE_ID) {
    return IndexIterator();
  }
  Page *page = FindLeafPage(key, page_id, false);
  LeafPage *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  int index = leaf->KeyIndex(key, processor_);
  IndexIterator iterator(leaf->GetPageId(), buffer_pool_manager_, index);
  return iterator;
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
IndexIterator BPlusTree::End() { return IndexIterator(INVALID_PAGE_ID, buffer_pool_manager_, 0); }

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 * Note: the leaf page is pinned, you need to unpin it after use.
 */
Page *BPlusTree::FindLeafPage(const GenericKey *key, page_id_t page_id, bool leftMost) {
  if (IsEmpty()) {
    return nullptr;
  }
  Page *page = buffer_pool_manager_->FetchPage(page_id);
  BPlusTreePage *tree_page = reinterpret_cast<BPlusTreePage *>(page);
  if (tree_page->IsLeafPage()) {
    return page;
  }
  InternalPage *internal_page = reinterpret_cast<InternalPage *>(page);
  if (leftMost) {
    page_id_t leftMostPageId = internal_page->ValueAt(0);
    buffer_pool_manager_->UnpinPage(page_id, false);
    return FindLeafPage(key, leftMostPageId, leftMost);
  }
  page_id_t pageId = internal_page->Lookup(key, processor_);
  buffer_pool_manager_->UnpinPage(page_id, false);
  return FindLeafPage(key, pageId, leftMost);
}

/*
 * Update/Insert root page id in header page(where page_id = 0, header_page is
 * defined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      default value is false. When set to true,
 * insert a record <index_name, current_page_id> into header page instead of
 * updating it.
 */
void BPlusTree::UpdateRootPageId(int insert_record) {
  IndexRootsPage *indexRootsPage =
      reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID));
  if (insert_record) {
    indexRootsPage->Insert(index_id_, root_page_id_);
  } else {
    indexRootsPage->Update(index_id_, root_page_id_);
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
}

/**
 * This method is used for debug only, You don't need to modify
 */
void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId()
        << ",Parent=" << leaf->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      Row keyrow(INVALID_ROWID);
      Schema *schema = nullptr;
      std::vector<Column *> columns = {
          new Column("int", TypeId::kTypeInt, 0, false, false),
      };
      Schema *table_schema = new Schema(columns);
      KeyManager KP(table_schema, 16);
      KP.DeserializeToKey(leaf->KeyAt(i), keyrow, table_schema);
      Field *row_value = keyrow.GetField(0);
      auto x = row_value->toString();
      out << "<TD>" << x << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId()
        << ",Parent=" << inner->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        std::vector<Column *> columns = {
            new Column("int", TypeId::kTypeInt, 0, false, false),
        };
        Row keyrow(INVALID_ROWID);
        Schema *table_schema = new Schema(columns);
        KeyManager KP(table_schema, 16);
        KP.DeserializeToKey(inner->KeyAt(i), keyrow, table_schema);
        Field *row_value = keyrow.GetField(0);
        auto x = row_value->toString();
        out << x;
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 */
void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
      bpm->UnpinPage(internal->ValueAt(i), false);
    }
  }
}

bool BPlusTree::Check() {
  bool all_unpinned = buffer_pool_manager_->CheckAllUnpinned();
  if (!all_unpinned) {
    LOG(ERROR) << "problem in page unpin" << endl;
  }
  return all_unpinned;
}