#ifndef LLM_CC_DOCTOR_H_
#define LLM_CC_DOCTOR_H_

namespace llmcc {

// Local preflight only: never downloads, loads weights or creates a context.
int RunDoctorCommand(int argc, char** argv);

}  // namespace llmcc

#endif  // LLM_CC_DOCTOR_H_
