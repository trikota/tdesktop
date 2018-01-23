/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_element.h"

#include "history/view/history_view_service_message.h"
#include "history/history_item_components.h"
#include "history/history_item.h"
#include "history/history_media.h"
#include "history/history_media_grouped.h"
#include "history/history.h"
#include "data/data_session.h"
#include "data/data_groups.h"
#include "data/data_media_types.h"
#include "lang/lang_keys.h"
#include "auth_session.h"
#include "layout.h"
#include "styles/style_history.h"

namespace HistoryView {
namespace {

// A new message from the same sender is attached to previous within 15 minutes.
constexpr int kAttachMessageToPreviousSecondsDelta = 900;

} // namespace

TextSelection UnshiftItemSelection(
		TextSelection selection,
		uint16 byLength) {
	return (selection == FullSelection)
		? selection
		: ::unshiftSelection(selection, byLength);
}

TextSelection ShiftItemSelection(
		TextSelection selection,
		uint16 byLength) {
	return (selection == FullSelection)
		? selection
		: ::shiftSelection(selection, byLength);
}

TextSelection UnshiftItemSelection(
		TextSelection selection,
		const Text &byText) {
	return UnshiftItemSelection(selection, byText.length());
}

TextSelection ShiftItemSelection(
		TextSelection selection,
		const Text &byText) {
	return ShiftItemSelection(selection, byText.length());
}

void UnreadBar::init(int count) {
	if (freezed) {
		return;
	}
	text = lng_unread_bar(lt_count, count);
	width = st::semiboldFont->width(text);
}

int UnreadBar::height() {
	return st::historyUnreadBarHeight + st::historyUnreadBarMargin;
}

int UnreadBar::marginTop() {
	return st::lineWidth + st::historyUnreadBarMargin;
}

void UnreadBar::paint(Painter &p, int y, int w) const {
	const auto bottom = y + height();
	y += marginTop();
	p.fillRect(
		0,
		y,
		w,
		height() - marginTop() - st::lineWidth,
		st::historyUnreadBarBg);
	p.fillRect(
		0,
		bottom - st::lineWidth,
		w,
		st::lineWidth,
		st::historyUnreadBarBorder);
	p.setFont(st::historyUnreadBarFont);
	p.setPen(st::historyUnreadBarFg);

	int left = st::msgServiceMargin.left();
	int maxwidth = w;
	if (Adaptive::ChatWide()) {
		maxwidth = qMin(
			maxwidth,
			st::msgMaxWidth
				+ 2 * st::msgPhotoSkip
				+ 2 * st::msgMargin.left());
	}
	w = maxwidth;

	const auto skip = st::historyUnreadBarHeight
		- 2 * st::lineWidth
		- st::historyUnreadBarFont->height;
	p.drawText(
		(w - width) / 2,
		y + (skip / 2) + st::historyUnreadBarFont->ascent,
		text);
}


void DateBadge::init(const QDateTime &date) {
	text = langDayOfMonthFull(date.date());
	width = st::msgServiceFont->width(text);
}

int DateBadge::height() const {
	return st::msgServiceMargin.top()
		+ st::msgServicePadding.top()
		+ st::msgServiceFont->height
		+ st::msgServicePadding.bottom()
		+ st::msgServiceMargin.bottom();
}

void DateBadge::paint(Painter &p, int y, int w) const {
	ServiceMessagePainter::paintDate(p, text, width, y, w);
}

Element::Element(
	not_null<ElementDelegate*> delegate,
	not_null<HistoryItem*> data)
: _delegate(delegate)
, _data(data)
, _context(delegate->elementContext()) {
	Auth().data().registerItemView(this);
	refreshMedia();
}

not_null<ElementDelegate*> Element::delegate() const {
	return _delegate;
}

not_null<HistoryItem*> Element::data() const {
	return _data;
}

HistoryMedia *Element::media() const {
	return _media.get();
}

Context Element::context() const {
	return _context;
}

int Element::y() const {
	return _y;
}

void Element::setY(int y) {
	_y = y;
}

int Element::marginTop() const {
	const auto item = data();
	auto result = 0;
	if (!isHiddenByGroup()) {
		if (isAttachedToPrevious()) {
			result += st::msgMarginTopAttached;
		} else {
			result += st::msgMargin.top();
		}
	}
	result += displayedDateHeight();
	if (const auto bar = Get<UnreadBar>()) {
		result += bar->height();
	}
	return result;
}

int Element::marginBottom() const {
	const auto item = data();
	return isHiddenByGroup() ? 0 : st::msgMargin.bottom();
}

bool Element::isUnderCursor() const {
	return (App::hoveredItem() == this);
}

void Element::setPendingResize() {
	_flags |= Flag::NeedsResize;
	if (_context == Context::History) {
		data()->_history->setHasPendingResizedItems();
	}
}

bool Element::pendingResize() const {
	return _flags & Flag::NeedsResize;
}

bool Element::isAttachedToPrevious() const {
	return _flags & Flag::AttachedToPrevious;
}

bool Element::isAttachedToNext() const {
	return _flags & Flag::AttachedToNext;
}

int Element::skipBlockWidth() const {
	return st::msgDateSpace + infoWidth() - st::msgDateDelta.x();
}

int Element::skipBlockHeight() const {
	return st::msgDateFont->height - st::msgDateDelta.y();
}

QString Element::skipBlock() const {
	return textcmdSkipBlock(skipBlockWidth(), skipBlockHeight());
}

int Element::infoWidth() const {
	return 0;
}

bool Element::isHiddenByGroup() const {
	return _flags & Flag::HiddenByGroup;
}

void Element::refreshMedia() {
	_flags &= ~Flag::HiddenByGroup;

	const auto item = data();
	const auto media = item->media();
	if (media && media->canBeGrouped()) {
		if (const auto group = Auth().data().groups().find(item)) {
			if (group->items.back() != item) {
				_media = nullptr;
				_flags |= Flag::HiddenByGroup;
			} else {
				_media = std::make_unique<HistoryGroupedMedia>(
					this,
					group->items);
				if (!pendingResize()) {
					Auth().data().requestViewResize(this);
				}
			}
			return;
		}
	}
	if (_data->media()) {
		_media = _data->media()->createView(this);
	} else {
		_media = nullptr;
	}
}

void Element::previousInBlocksChanged() {
	recountDisplayDateInBlocks();
	recountAttachToPreviousInBlocks();
}

void Element::nextInBlocksRemoved() {
	setAttachToNext(false);
}

void Element::refreshDataId() {
	if (const auto media = this->media()) {
		media->refreshParentId(data());
	}
}

bool Element::computeIsAttachToPrevious(not_null<Element*> previous) {
	const auto item = data();
	if (!Has<DateBadge>() && !Has<UnreadBar>()) {
		const auto prev = previous->data();
		const auto possible = !item->serviceMsg() && !prev->serviceMsg()
			&& !item->isEmpty() && !prev->isEmpty()
			&& (qAbs(prev->date.secsTo(item->date)) < kAttachMessageToPreviousSecondsDelta)
			&& (_context == Context::Feed
				|| (!item->isPost() && !prev->isPost()));
		if (possible) {
			if (item->history()->peer->isSelf()) {
				return prev->senderOriginal() == item->senderOriginal()
					&& (prev->Has<HistoryMessageForwarded>() == item->Has<HistoryMessageForwarded>());
			} else {
				return prev->from() == item->from();
			}
		}
	}
	return false;
}

void Element::destroyUnreadBar() {
	if (!Has<UnreadBar>()) {
		return;
	}
	RemoveComponents(UnreadBar::Bit());
	Auth().data().requestViewResize(this);
	if (data()->mainView() == this) {
		recountAttachToPreviousInBlocks();
	}
}

void Element::setUnreadBarCount(int count) {
	Expects(count > 0);

	const auto changed = AddComponents(UnreadBar::Bit());
	const auto bar = Get<UnreadBar>();
	if (bar->freezed) {
		return;
	}
	bar->init(count);
	if (changed) {
		if (data()->mainView() == this) {
			recountAttachToPreviousInBlocks();
		}
		Auth().data().requestViewResize(this);
	} else {
		Auth().data().requestViewRepaint(this);
	}
}

void Element::setUnreadBarFreezed() {
	if (const auto bar = Get<UnreadBar>()) {
		bar->freezed = true;
	}
}

int Element::displayedDateHeight() const {
	if (auto date = Get<DateBadge>()) {
		return date->height();
	}
	return 0;
}

bool Element::displayDate() const {
	return Has<DateBadge>();
}

bool Element::isInOneDayWithPrevious() const {
	return !data()->isEmpty() && !displayDate();
}

void Element::recountAttachToPreviousInBlocks() {
	auto attachToPrevious = false;
	if (const auto previous = previousInBlocks()) {
		attachToPrevious = computeIsAttachToPrevious(previous);
		previous->setAttachToNext(attachToPrevious);
	}
	setAttachToPrevious(attachToPrevious);
}

void Element::recountDisplayDateInBlocks() {
	setDisplayDate([&] {
		const auto item = data();
		if (item->isEmpty()) {
			return false;
		}

		if (auto previous = previousInBlocks()) {
			const auto prev = previous->data();
			return prev->isEmpty() || (prev->date.date() != item->date.date());
		}
		return true;
	}());
}

QSize Element::countOptimalSize() {
	return performCountOptimalSize();
}

QSize Element::countCurrentSize(int newWidth) {
	if (_flags & Flag::NeedsResize) {
		_flags &= ~Flag::NeedsResize;
		initDimensions();
	}
	return performCountCurrentSize(newWidth);
}

void Element::setDisplayDate(bool displayDate) {
	const auto item = data();
	if (displayDate && !Has<DateBadge>()) {
		AddComponents(DateBadge::Bit());
		Get<DateBadge>()->init(item->date);
		setPendingResize();
	} else if (!displayDate && Has<DateBadge>()) {
		RemoveComponents(DateBadge::Bit());
		setPendingResize();
	}
}

void Element::setAttachToNext(bool attachToNext) {
	if (attachToNext && !(_flags & Flag::AttachedToNext)) {
		_flags |= Flag::AttachedToNext;
		setPendingResize();
	} else if (!attachToNext && (_flags & Flag::AttachedToNext)) {
		_flags &= ~Flag::AttachedToNext;
		setPendingResize();
	}
}

void Element::setAttachToPrevious(bool attachToPrevious) {
	if (attachToPrevious && !(_flags & Flag::AttachedToPrevious)) {
		_flags |= Flag::AttachedToPrevious;
		setPendingResize();
	} else if (!attachToPrevious && (_flags & Flag::AttachedToPrevious)) {
		_flags &= ~Flag::AttachedToPrevious;
		setPendingResize();
	}
}

bool Element::displayFromPhoto() const {
	return false;
}

bool Element::hasFromPhoto() const {
	return false;
}

bool Element::hasFromName() const {
	return false;
}

bool Element::displayFromName() const {
	return false;
}

bool Element::displayForwardedFrom() const {
	return false;
}

bool Element::hasOutLayout() const {
	return false;
}

bool Element::drawBubble() const {
	return false;
}

bool Element::hasBubble() const {
	return false;
}

bool Element::hasFastReply() const {
	return false;
}

bool Element::displayFastReply() const {
	return false;
}

bool Element::displayRightAction() const {
	return false;
}

void Element::drawRightAction(
	Painter &p,
	int left,
	int top,
	int outerWidth) const {
}

ClickHandlerPtr Element::rightActionLink() const {
	return ClickHandlerPtr();
}

bool Element::displayEditedBadge() const {
	return false;
}

QDateTime Element::displayedEditDate() const {
	return QDateTime();
}

bool Element::hasVisibleText() const {
	return false;
}

HistoryBlock *Element::block() {
	return _block;
}

const HistoryBlock *Element::block() const {
	return _block;
}

void Element::attachToBlock(not_null<HistoryBlock*> block, int index) {
	Expects(!_data->isLogEntry());
	Expects(_block == nullptr);
	Expects(_indexInBlock < 0);
	Expects(index >= 0);

	_block = block;
	_indexInBlock = index;
	_data->setMainView(this);
	previousInBlocksChanged();
}

void Element::removeFromBlock() {
	Expects(_block != nullptr);

	_block->remove(this);
}

void Element::refreshInBlock() {
	Expects(_block != nullptr);

	_block->refreshView(this);
}

void Element::setIndexInBlock(int index) {
	Expects(_block != nullptr);
	Expects(index >= 0);

	_indexInBlock = index;
}

int Element::indexInBlock() const {
	Expects((_indexInBlock >= 0) == (_block != nullptr));
	Expects((_block == nullptr) || (_block->messages[_indexInBlock].get() == this));

	return _indexInBlock;
}

Element *Element::previousInBlocks() const {
	if (_block && _indexInBlock >= 0) {
		if (_indexInBlock > 0) {
			return _block->messages[_indexInBlock - 1].get();
		}
		if (auto previous = _block->previousBlock()) {
			Assert(!previous->messages.empty());
			return previous->messages.back().get();
		}
	}
	return nullptr;
}

Element *Element::nextInBlocks() const {
	if (_block && _indexInBlock >= 0) {
		if (_indexInBlock + 1 < _block->messages.size()) {
			return _block->messages[_indexInBlock + 1].get();
		}
		if (auto next = _block->nextBlock()) {
			Assert(!next->messages.empty());
			return next->messages.front().get();
		}
	}
	return nullptr;
}

void Element::drawInfo(
	Painter &p,
	int right,
	int bottom,
	int width,
	bool selected,
	InfoDisplayType type) const {
}

bool Element::pointInTime(
		int right,
		int bottom,
		QPoint point,
		InfoDisplayType type) const {
	return false;
}

TextSelection Element::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	return selection;
}

void Element::clickHandlerActiveChanged(
		const ClickHandlerPtr &handler,
		bool active) {
	if (const auto markup = _data->Get<HistoryMessageReplyMarkup>()) {
		if (const auto keyboard = markup->inlineKeyboard.get()) {
			keyboard->clickHandlerActiveChanged(handler, active);
		}
	}
	App::hoveredLinkItem(active ? this : nullptr);
	Auth().data().requestViewRepaint(this);
	if (const auto media = this->media()) {
		media->clickHandlerActiveChanged(handler, active);
	}
}

void Element::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (const auto markup = _data->Get<HistoryMessageReplyMarkup>()) {
		if (const auto keyboard = markup->inlineKeyboard.get()) {
			keyboard->clickHandlerPressedChanged(handler, pressed);
		}
	}
	App::pressedLinkItem(pressed ? this : nullptr);
	Auth().data().requestViewRepaint(this);
	if (const auto media = this->media()) {
		media->clickHandlerPressedChanged(handler, pressed);
	}
}

Element::~Element() {
	Auth().data().unregisterItemView(this);
}

} // namespace HistoryView
