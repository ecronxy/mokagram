// Mokagram: окно жалобы в стиле окон mokaGram.
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Api {
struct ReportResult;
} // namespace Api

namespace Data {
struct ReportInput;
} // namespace Data

namespace Mokagram::Report {

void FlowBox(
	not_null<Ui::GenericBox*> box,
	const Api::ReportResult &result,
	const Data::ReportInput &input,
	Fn<void(Data::ReportInput)> choose);

} // namespace Mokagram::Report
